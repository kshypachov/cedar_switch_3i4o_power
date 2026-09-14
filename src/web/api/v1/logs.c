/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1: log sources, a bounded page of records under a cursor, and the
 * export.
 *
 * Contract: "Логи" in api-contract.md. The rings, the cursor and the filters
 * are log-store's; this file reads the query, walks a reader and writes the
 * schemas. Decisions the contract leaves to the device:
 *
 * - **A page is bounded by bytes as well as by `limit`.** It is built in the
 *   request's response buffer (CONFIG_WEB_API_RESPONSE_BODY_MAX). A record that
 *   would not leave room for the rest of the document is put back - the writer
 *   and the reader are both rolled back - and the page ends with has_more=true
 *   and next_cursor at that record. The mock applies the same budget.
 *
 * - **A page looks at no more than CONFIG_LOG_STORE_SCAN_BUDGET records.** A
 *   filter that matches almost nothing still answers promptly; the page may
 *   then be empty with has_more=true, and the cursor continues the scan.
 *
 * - **has_more is exact.** After `limit` records the reader looks one record
 *   further and puts it back, so a client that got a full page is not sent
 *   back for an empty one.
 *
 * - **A cursor of another boot is not an error**: the page is the current tail,
 *   as without a cursor, with the new boot_id and gap=true (the contract's
 *   "текущий хвост"). An empty, damaged or other-filter cursor is 400
 *   invalid_cursor. Query values that break their schema are 400
 *   invalid_query, checked before the cursor, in the mock's order.
 *
 * - **A module filter longer than any stored module** (64 bytes; the schema
 *   counts 64 code points) is valid and matches nothing.
 *
 * - **`dropped_count`** is the records of the selected sources that never
 *   reached a ring. Records a ring overwrote are what `gap` reports.
 *
 * - **The export is streamed.** It fixes the upper sequence number when it
 *   starts, positions a reader on the newest `max_records` matching records,
 *   and writes as many lines as fit the response buffer per piece; the server
 *   sends each piece before asking for the next, so RAM holds one piece. A
 *   record the ring overwrote while the export was being sent becomes an
 *   explicit gap line. The HTTP server's one thread is busy for the whole
 *   export (measured in reports/p5); the log producers are not - a reader holds
 *   log-store's lock for one record copy at a time.
 *
 * Handlers run on the HTTP server's one thread, which is why the large working
 * values below are static rather than on its stack.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <log_store/log_store.h>

#include "v1_internal.h"

#if defined(CONFIG_LOG_STORE_SCAN_BUDGET)
#define SCAN_BUDGET CONFIG_LOG_STORE_SCAN_BUDGET
#else
#define SCAN_BUDGET 2048
#endif

#define LIMIT_MAX          100
#define EXPORT_MAX_RECORDS 2000
/* One piece of an export looks at no more than this many records. */
#define EXPORT_SCAN_PIECE  SCAN_BUDGET

/* Room the end of a page needs after the last record. */
#define PAGE_TAIL_RESERVE                                                                          \
	(sizeof("],\"next_cursor\":\"\",\"has_more\":false,\"gap\":false,"                        \
		"\"dropped_count\":\"18446744073709551615\"}") +                                   \
	 LOG_STORE_CURSOR_MAX + 8U)

#define GAP_MESSAGE "records were overwritten before they could be exported"

const char *const v1_log_records_query[] = {
	"source", "min_level", "module", "contains", "cursor", "limit", NULL,
};

const char *const v1_log_export_query[] = {
	"source", "min_level", "module", "contains", "format", "max_records", NULL,
};

/* -- query --------------------------------------------------------------- */

static size_t code_points(const char *s)
{
	size_t n = 0;

	for (; *s != '\0'; s++) {
		n += ((uint8_t)*s & 0xC0U) != 0x80U ? 1U : 0U;
	}

	return n;
}

static bool reject_query(struct web_api_call *call, const char *message)
{
	web_api_reject(call, API_ERR_INVALID_QUERY, message);
	return false;
}

/* An integer parameter in [min, max], or @p fallback when absent. */
static bool query_int(struct web_api_call *call, const char *name, int min, int max, int fallback,
		      int *out)
{
	char text[12];
	int rc = web_api_query_get(call->req, name, text, sizeof(text));
	long v = 0;

	if (rc == -ENOENT) {
		*out = fallback;
		return true;
	}
	if (rc <= 0) {
		return reject_query(call, "An integer query parameter is not valid");
	}
	for (int i = 0; i < rc; i++) {
		if (text[i] < '0' || text[i] > '9') {
			return reject_query(call, "An integer query parameter is not an integer");
		}
		v = v * 10 + (text[i] - '0');
	}
	if (v < min || v > max) {
		return reject_query(call, "An integer query parameter is out of range");
	}
	*out = (int)v;

	return true;
}

/* Filters shared by the page and the export; @p mask gets the selected sources. */
static bool query_filter(struct web_api_call *call, struct log_store_filter *f, uint8_t *mask)
{
	static const char *const levels[] = {"debug", "info", "warning", "error"};
	static char text[LOG_STORE_CONTAINS_MAX + 1];
	int rc;

	memset(f, 0, sizeof(*f));
	f->sources = LOG_STORE_SOURCES_ALL;

	rc = web_api_query_get(call->req, "source", text, sizeof(text));
	if (rc >= 0) {
		if (strcmp(text, "stm32") == 0) {
			f->sources = LOG_STORE_SOURCE_BIT(LOG_STORE_STM32);
		} else if (strcmp(text, "esp32") == 0) {
			f->sources = LOG_STORE_SOURCE_BIT(LOG_STORE_ESP32);
		} else if (strcmp(text, "all") != 0) {
			return reject_query(call, "source must be all, stm32 or esp32");
		}
	} else if (rc != -ENOENT) {
		return reject_query(call, "source must be all, stm32 or esp32");
	}
	*mask = f->sources;

	rc = web_api_query_get(call->req, "min_level", text, sizeof(text));
	if (rc >= 0) {
		size_t i;

		for (i = 0; i < ARRAY_SIZE(levels); i++) {
			if (strcmp(text, levels[i]) == 0) {
				f->min_level = (enum log_store_level)(LOG_STORE_LEVEL_DEBUG + i);
				break;
			}
		}
		if (i == ARRAY_SIZE(levels)) {
			return reject_query(call, "min_level must be debug, info, warning or error");
		}
	} else if (rc != -ENOENT) {
		return reject_query(call, "min_level must be debug, info, warning or error");
	}

	rc = web_api_query_get(call->req, "module", text, sizeof(text));
	if (rc >= 0) {
		if (code_points(text) > LOG_STORE_MODULE_MAX) {
			return reject_query(call, "module is longer than 64 characters");
		}
		if ((size_t)rc > LOG_STORE_MODULE_MAX) {
			/* Valid, and longer than any module a record can carry. */
			f->sources = 0U;
		} else {
			f->has_module = true;
			memcpy(f->module, text, (size_t)rc + 1U);
		}
	} else if (rc != -ENOENT) {
		return reject_query(call, "module is longer than 64 characters");
	}

	rc = web_api_query_get(call->req, "contains", text, sizeof(text));
	if (rc >= 0) {
		if (code_points(text) > 128U) {
			return reject_query(call, "contains is longer than 128 characters");
		}
		if (rc > 0) {
			f->has_contains = true;
			memcpy(f->contains, text, (size_t)rc + 1U);
		}
	} else if (rc != -ENOENT) {
		return reject_query(call, "contains is longer than 128 characters");
	}

	return true;
}

/* -- a record ------------------------------------------------------------ */

static void write_record(struct web_json_writer *w, const struct log_store_record *r,
			 const char *boot_id)
{
	web_json_object_begin(w);
	web_json_key(w, "source");
	web_json_string(w, log_store_source_str(r->source));
	web_json_key(w, "boot_id");
	web_json_string(w, boot_id);
	web_json_key(w, "source_generation");
	web_json_int(w, r->generation);
	web_json_key(w, "seq");
	web_json_decimal(w, r->seq);
	web_json_key(w, "uptime_ms");
	web_json_decimal(w, r->uptime_ms);
	web_json_key(w, "wall_time");
	/* No clock source is synchronised on this device. */
	web_json_null(w);
	web_json_key(w, "level");
	web_json_string_or_null(w, log_store_level_str(r->level));
	web_json_key(w, "module");
	web_json_string_or_null(w, r->has_module ? r->module : NULL);
	web_json_key(w, "message");
	web_json_string(w, r->text);
	web_json_key(w, "truncated");
	web_json_bool(w, r->truncated);
	web_json_key(w, "kind");
	web_json_string(w, log_store_kind_str(r->kind));
	web_json_object_end(w);
}

/* -- sources -------------------------------------------------------------- */

static void source_item(struct web_json_writer *w, enum log_store_source source, bool available,
			const char *reason, uint32_t generation)
{
	web_json_object_begin(w);
	web_json_key(w, "id");
	web_json_string(w, log_store_source_str(source));
	web_json_key(w, "available");
	web_json_bool(w, available);
	web_json_key(w, "reason");
	web_json_string_or_null(w, available ? NULL : reason);
	web_json_key(w, "generation");
	web_json_int(w, generation);
	web_json_key(w, "dropped_count");
	web_json_decimal(w, log_store_dropped(LOG_STORE_SOURCE_BIT(source)));
	web_json_object_end(w);
}

const char *v1_esp32_logs_unavailable_reason(uint32_t *generation)
{
	struct coprocessor_status cp;
	const char *reason;

	coprocessor_manager_get_status(&cp);
	if (generation != NULL) {
		*generation = cp.generation;
	}
	if (!log_store_ready()) {
		return "not_started";
	}
	reason = coprocessor_logs_unavailable_reason(cp.uart_mode);

	return reason;
}

void v1_get_log_sources(struct web_api_call *call)
{
	struct web_json_writer *w = web_api_json(call);
	uint32_t generation = 0;
	const char *esp32_reason = v1_esp32_logs_unavailable_reason(&generation);

	web_json_object_begin(w);
	web_json_key(w, "items");
	web_json_array_begin(w);
	/* The STM32's restarts are boot_id changes; it has no generation of its own. */
	source_item(w, LOG_STORE_STM32, log_store_ready(), "not_started", 0U);
	source_item(w, LOG_STORE_ESP32, esp32_reason == NULL, esp32_reason, generation);
	web_json_array_end(w);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

/* -- a page ---------------------------------------------------------------- */

static struct log_store_reader page_reader;
static struct log_store_reader page_saved;
static struct log_store_record page_record;
static char page_cursor[512 + 1];

void v1_get_log_records(struct web_api_call *call)
{
	const char *boot_id = v1_identity()->boot_id;
	const uint32_t tag = log_store_boot_tag(boot_id);
	struct log_store_filter filter;
	struct web_json_writer saved_writer;
	struct web_json_writer *w;
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	char next_cursor[LOG_STORE_CURSOR_MAX];
	uint8_t mask;
	int limit;
	int count = 0;
	bool has_more = false;
	bool gap = false;
	int rc;

	if (!query_filter(call, &filter, &mask) ||
	    !query_int(call, "limit", 1, LIMIT_MAX, LIMIT_MAX, &limit)) {
		return;
	}
	rc = web_api_query_get(call->req, "cursor", page_cursor, sizeof(page_cursor));
	if (rc == -ENOSPC) {
		web_api_reject(call, API_ERR_INVALID_QUERY, "cursor is longer than 512 characters");
		return;
	}

	if (rc == -ENOENT) {
		log_store_reader_tail(&page_reader, &filter, (uint32_t)limit, SCAN_BUDGET);
	} else {
		rc = (rc > 0) ? log_store_cursor_decode(page_cursor, tag, &filter, pos) : -EINVAL;
		if (rc == -ESTALE) {
			log_store_reader_tail(&page_reader, &filter, (uint32_t)limit, SCAN_BUDGET);
			gap = true;
		} else if (rc != 0) {
			web_api_reject(call, API_ERR_INVALID_CURSOR,
				       "The cursor is not one this device issued for these filters; "
				       "drop it and read the tail");
			return;
		} else {
			log_store_reader_at(&page_reader, &filter, pos);
		}
	}
	page_reader.scanned = 0U;

	w = web_api_json(call);
	web_json_object_begin(w);
	web_json_key(w, "boot_id");
	web_json_string(w, boot_id);
	web_json_key(w, "items");
	web_json_array_begin(w);

	for (;;) {
		const uint32_t left =
			SCAN_BUDGET > page_reader.scanned ? SCAN_BUDGET - page_reader.scanned : 0U;
		enum log_store_read_result res;

		if (left == 0U) {
			has_more = true;
			break;
		}
		page_saved = page_reader;
		res = log_store_read(&page_reader, &page_record, left);
		if (count == limit) {
			/* Looked one further only to answer has_more; put it back. */
			has_more = res != LOG_STORE_READ_END;
			page_reader = page_saved;
			break;
		}
		if (res == LOG_STORE_READ_END) {
			break;
		}
		if (res == LOG_STORE_READ_BUDGET) {
			has_more = true;
			break;
		}
		saved_writer = *w;
		write_record(w, &page_record, boot_id);
		if (web_json_writer_room(w) < PAGE_TAIL_RESERVE) {
			web_json_writer_rollback(w, &saved_writer);
			page_reader = page_saved;
			has_more = true;
			break;
		}
		count++;
	}
	gap = gap || page_reader.gap;

	web_json_array_end(w);
	rc = log_store_cursor_encode(&page_reader, tag, next_cursor, sizeof(next_cursor));
	web_json_key(w, "next_cursor");
	web_json_string(w, rc >= 0 ? next_cursor : "");
	web_json_key(w, "has_more");
	web_json_bool(w, has_more);
	web_json_key(w, "gap");
	web_json_bool(w, gap);
	web_json_key(w, "dropped_count");
	web_json_decimal(w, log_store_dropped(mask));
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

/* -- the export ------------------------------------------------------------ */

struct export_state {
	struct log_store_reader reader;
	const char *boot_id;
	uint32_t remaining;
	bool text;
	bool header_done;
};

BUILD_ASSERT(sizeof(struct export_state) <= CONFIG_WEB_API_STREAM_STATE_MAX,
	     "the export's state must fit CONFIG_WEB_API_STREAM_STATE_MAX");

static struct log_store_record export_record;
static struct log_store_reader export_saved;

/* Append @p n bytes, or report that they do not fit. */
static bool put(char *buf, size_t cap, size_t *len, const char *text, size_t n)
{
	if (*len + n > cap) {
		return false;
	}
	memcpy(&buf[*len], text, n);
	*len += n;
	return true;
}

static bool put_str(char *buf, size_t cap, size_t *len, const char *text)
{
	return put(buf, cap, len, text, strlen(text));
}

static bool text_line(char *buf, size_t cap, size_t *len, const struct log_store_record *r)
{
	char head[96];
	const char *level = log_store_level_str(r->level);
	int n;

	n = snprintf(head, sizeof(head), "%llu %s %s %s ", (unsigned long long)r->uptime_ms,
		     log_store_source_str(r->source), level != NULL ? level : "-",
		     r->has_module ? r->module : "-");
	if (n < 0 || !put(buf, cap, len, head, MIN((size_t)n, sizeof(head) - 1U))) {
		return false;
	}
	for (uint16_t i = 0; i < r->text_len; i++) {
		const char c = r->text[i];

		if (c == '\n' || c == '\r') {
			if (!put(buf, cap, len, "\\n", 2)) {
				return false;
			}
			if (c == '\r' && i + 1U < r->text_len && r->text[i + 1U] == '\n') {
				i++;
			}
		} else if (!put(buf, cap, len, &c, 1)) {
			return false;
		}
	}
	return put(buf, cap, len, "\n", 1);
}

static bool json_line(char *buf, size_t cap, size_t *len, const struct log_store_record *r,
		      const char *boot_id)
{
	struct web_json_writer w;
	int n;

	web_json_writer_init(&w, &buf[*len], cap - *len);
	write_record(&w, r, boot_id);
	n = web_json_writer_finish(&w);
	if (n < 0) {
		return false;
	}
	*len += (size_t)n;
	return put(buf, cap, len, "\n", 1);
}

/*
 * A record of its own, before the first record after the gap. When the ring
 * took everything up to the end of the snapshot there is no such record: the
 * gap then carries the snapshot's upper sequence number - the first record
 * after the gap, had it been included - the export's first source, and the
 * time it was noticed.
 */
static bool gap_line(struct export_state *st, char *buf, size_t cap, size_t *len,
		     const struct log_store_record *next)
{
	struct log_store_record gap;

	if (st->text) {
		return put_str(buf, cap, len, "# gap: " GAP_MESSAGE "\n");
	}
	memset(&gap, 0, sizeof(gap));
	if (next != NULL) {
		gap.source = next->source;
		gap.generation = next->generation;
		gap.seq = next->seq;
		gap.uptime_ms = next->uptime_ms;
	} else {
		gap.source = (st->reader.filter.sources & LOG_STORE_SOURCE_BIT(LOG_STORE_STM32))
				     ? LOG_STORE_STM32
				     : LOG_STORE_ESP32;
		gap.seq = st->reader.upper_seq;
		gap.uptime_ms = (uint64_t)k_uptime_get();
	}
	gap.level = LOG_STORE_LEVEL_NONE;
	gap.kind = LOG_STORE_KIND_GAP;
	gap.text_len = sizeof(GAP_MESSAGE) - 1U;
	memcpy(gap.text, GAP_MESSAGE, sizeof(GAP_MESSAGE));

	return json_line(buf, cap, len, &gap, st->boot_id);
}

static int export_next(void *state, char *buf, size_t cap)
{
	struct export_state *st = state;
	size_t len = 0;

	if (st->text && !st->header_done) {
		char head[128];
		int n = snprintf(head, sizeof(head),
				 "# cedar logs %s: bounded snapshot, nothing from before this boot; "
				 "gaps are marked\n",
				 st->boot_id);

		(void)put(buf, cap, &len, head, MIN((size_t)MAX(n, 0), sizeof(head) - 1U));
		st->header_done = true;
	}

	while (st->remaining > 0U) {
		enum log_store_read_result res;
		size_t mark = len;
		bool fits;

		export_saved = st->reader;
		st->reader.scanned = 0U;
		res = log_store_read(&st->reader, &export_record, EXPORT_SCAN_PIECE);
		if (res == LOG_STORE_READ_END) {
			if (st->reader.gap) {
				/* Overwritten to the end of the snapshot. */
				if (!gap_line(st, buf, cap, &len, NULL)) {
					st->reader = export_saved;
					len = mark;
					break;
				}
				st->reader.gap = false;
			}
			st->remaining = 0U;
			break;
		}
		if (res == LOG_STORE_READ_BUDGET) {
			if (len > 0U) {
				break;
			}
			continue;
		}
		fits = !st->reader.gap || gap_line(st, buf, cap, &len, &export_record);
		fits = fits && (st->text ? text_line(buf, cap, &len, &export_record)
					 : json_line(buf, cap, &len, &export_record, st->boot_id));
		if (!fits) {
			st->reader = export_saved;
			len = mark;
			if (len == 0U) {
				/* A single record larger than a whole piece cannot happen with
				 * the buffer sizes checked at build time; stop cleanly. */
				return -ENOSPC;
			}
			break;
		}
		st->reader.gap = false;
		st->remaining--;
	}

	return (int)len;
}

BUILD_ASSERT(CONFIG_WEB_API_RESPONSE_BODY_MAX >= 4096,
	     "an export piece must hold the longest record line");

void v1_export_logs(struct web_api_call *call)
{
	struct log_store_filter filter;
	struct export_state *st;
	char format[8];
	uint8_t mask;
	int max_records;
	bool text = false;
	int rc;

	if (!query_filter(call, &filter, &mask) ||
	    !query_int(call, "max_records", 1, EXPORT_MAX_RECORDS, EXPORT_MAX_RECORDS,
		       &max_records)) {
		return;
	}
	rc = web_api_query_get(call->req, "format", format, sizeof(format));
	if (rc >= 0) {
		if (strcmp(format, "text") == 0) {
			text = true;
		} else if (strcmp(format, "ndjson") != 0) {
			web_api_reject(call, API_ERR_INVALID_QUERY, "format must be ndjson or text");
			return;
		}
	} else if (rc != -ENOENT) {
		web_api_reject(call, API_ERR_INVALID_QUERY, "format must be ndjson or text");
		return;
	}

	st = web_api_reply_stream(call, 200,
				  text ? "text/plain; charset=utf-8" : "application/x-ndjson",
				  sizeof(*st), export_next, NULL);
	if (st == NULL) {
		return;
	}
	web_api_add_header(call, "Content-Disposition",
			   text ? "attachment; filename=\"cedar-logs.txt\""
				: "attachment; filename=\"cedar-logs.ndjson\"");

	const uint64_t upper = log_store_next_seq();

	/* The whole ring may be walked to find the newest max_records matches. */
	log_store_reader_tail(&st->reader, &filter, (uint32_t)max_records, UINT32_MAX);
	st->reader.upper_seq = upper;
	st->reader.gap = false;
	st->boot_id = v1_identity()->boot_id;
	st->remaining = (uint32_t)max_records;
	st->text = text;
}
