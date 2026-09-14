/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr log backend that feeds log-store. Design in
 * include/zephyr_log_source/zephyr_log_source.h.
 *
 * Nothing in this file may log.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_msg.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/sys/atomic.h>

#include <log_store/log_store.h>
#include <zephyr_log_source/zephyr_log_source.h>

#define RAW_MAX CONFIG_ZEPHYR_LOG_SOURCE_RAW_MAX

/* No timestamp, level, colours or source: the message text alone. LF line
 * ends, so a hexdump keeps its lines; the final LF is removed afterwards. */
#define OUTPUT_FLAGS (LOG_OUTPUT_FLAG_SKIP_SOURCE | LOG_OUTPUT_FLAG_CRLF_LFONLY)

/* -- sanitising ------------------------------------------------------------ */

struct text_writer {
	char *out;
	size_t cap;
	size_t len;
	bool full;
	bool last_was_replacement;
};

static void put(struct text_writer *w, const uint8_t *bytes, size_t n, bool replacement)
{
	if (w->full) {
		return;
	}
	if (replacement && w->last_was_replacement) {
		return;
	}
	if (w->len + n > w->cap) {
		w->full = true;
		return;
	}
	memcpy(&w->out[w->len], bytes, n);
	w->len += n;
	w->last_was_replacement = replacement;
}

static void put_replacement(struct text_writer *w)
{
	static const uint8_t fffd[3] = {0xEF, 0xBF, 0xBD};

	put(w, fffd, sizeof(fffd), true);
}

/* Length of the well-formed UTF-8 sequence at @p s (Unicode table 3-7), or 0. */
static size_t utf8_sequence(const uint8_t *s, size_t n)
{
	uint8_t b = s[0];
	size_t need;
	uint8_t lo = 0x80;
	uint8_t hi = 0xBF;

	if (b >= 0xC2 && b <= 0xDF) {
		need = 2;
	} else if (b >= 0xE0 && b <= 0xEF) {
		need = 3;
		lo = (b == 0xE0) ? 0xA0 : 0x80;
		hi = (b == 0xED) ? 0x9F : 0xBF;
	} else if (b >= 0xF0 && b <= 0xF4) {
		need = 4;
		lo = (b == 0xF0) ? 0x90 : 0x80;
		hi = (b == 0xF4) ? 0x8F : 0xBF;
	} else {
		return 0;
	}
	if (n < need || s[1] < lo || s[1] > hi) {
		return 0;
	}
	for (size_t k = 2; k < need; k++) {
		if ((s[k] & 0xC0) != 0x80) {
			return 0;
		}
	}
	return need;
}

size_t zephyr_log_source_sanitise(const uint8_t *in, size_t len, char *out, size_t cap, bool *cut)
{
	struct text_writer w = {.out = out, .cap = cap};
	size_t i = 0;

	while (i < len && !w.full) {
		uint8_t b = in[i];

		if (b == '\r') {
			i++;
		} else if (b == '\n' || b == '\t' || (b >= 0x20 && b < 0x7F)) {
			put(&w, &in[i], 1, false);
			i++;
		} else if (b < 0x80) {
			/* Another control character, or DEL. */
			put_replacement(&w);
			i++;
		} else {
			size_t n = utf8_sequence(&in[i], len - i);

			if (n == 0) {
				/* One byte at a time: the next may start a valid sequence,
				 * and a run of these collapses anyway. */
				put_replacement(&w);
				i++;
			} else {
				put(&w, &in[i], n, false);
				i += n;
			}
		}
	}

	if (w.full) {
		*cut = true;
	}
	while (w.len > 0 && out[w.len - 1] == '\n') {
		w.len--;
	}
	return w.len;
}

/* -- the backend ----------------------------------------------------------- */

static atomic_t panicked;
static atomic_t stat_processed;
static atomic_t stat_stored;
static atomic_t stat_dropped;
static atomic_t stat_refused;
static atomic_t stat_after_panic;
static atomic_t stat_truncated;

/* Used only on the log thread, and never after a panic. */
static uint8_t raw[RAW_MAX];
static size_t raw_len;
static bool raw_cut;
static char text[LOG_STORE_TEXT_MAX];
static uint8_t output_buf[128];

static int collect(uint8_t *data, size_t length, void *ctx)
{
	size_t room = sizeof(raw) - raw_len;
	size_t n = MIN(length, room);

	ARG_UNUSED(ctx);
	memcpy(&raw[raw_len], data, n);
	raw_len += n;
	if (n < length) {
		raw_cut = true;
	}
	/* Report everything consumed: the output must not retry what is cut. */
	return (int)length;
}

LOG_OUTPUT_DEFINE(zephyr_log_source_output, collect, output_buf, sizeof(output_buf));

static enum log_store_level store_level(uint8_t level)
{
	switch (level) {
	case LOG_LEVEL_ERR:
		return LOG_STORE_LEVEL_ERROR;
	case LOG_LEVEL_WRN:
		return LOG_STORE_LEVEL_WARNING;
	case LOG_LEVEL_INF:
		return LOG_STORE_LEVEL_INFO;
	case LOG_LEVEL_DBG:
		return LOG_STORE_LEVEL_DEBUG;
	default:
		/* LOG_LEVEL_INTERNAL_RAW_STRING: printk and LOG_PRINTK. */
		return LOG_STORE_LEVEL_NONE;
	}
}

static void count_loss(void)
{
	log_store_count_loss(LOG_STORE_STM32, LOG_STORE_LOSS_BACKEND, 1);
}

static void process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	struct log_msg *m = &msg->log;
	uint8_t level;
	const char *module = NULL;
	uint8_t *package;
	uint8_t *data;
	size_t package_len;
	size_t data_len;
	bool truncated;
	size_t text_len;

	ARG_UNUSED(backend);
	atomic_inc(&stat_processed);

	if (atomic_get(&panicked) != 0) {
		atomic_inc(&stat_after_panic);
		count_loss();
		return;
	}

	level = log_msg_get_level(m);
	if (level != LOG_LEVEL_INTERNAL_RAW_STRING) {
		int16_t source_id = log_msg_get_source_id(m);

		if (source_id >= 0) {
			module = log_source_name_get(log_msg_get_domain(m), (uint32_t)source_id);
		}
	}

	package = log_msg_get_package(m, &package_len);
	data = log_msg_get_data(m, &data_len);

	raw_len = 0;
	raw_cut = false;
	log_output_process(&zephyr_log_source_output, 0, NULL, NULL, NULL, 0, level,
			   package_len > 0 ? package : NULL, data, data_len, OUTPUT_FLAGS);

	truncated = raw_cut;
	text_len = zephyr_log_source_sanitise(raw, raw_len, text, sizeof(text), &truncated);

	const struct log_store_entry entry = {
		.source = LOG_STORE_STM32,
		.level = store_level(level),
		.kind = LOG_STORE_KIND_MESSAGE,
		.truncated = truncated,
		/* The STM32 has no generation of its own; boot_id tells boots apart. */
		.generation = 0,
		.uptime_ms = log_output_timestamp_to_us(log_msg_get_timestamp(m)) / 1000U,
		.module = module,
		.module_len = module != NULL ? strlen(module) : 0U,
		.text = text,
		.text_len = text_len,
	};

	if (log_store_append(&entry) != 0) {
		atomic_inc(&stat_refused);
		count_loss();
		return;
	}
	atomic_inc(&stat_stored);
	if (truncated) {
		atomic_inc(&stat_truncated);
	}
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);
	atomic_add(&stat_dropped, (atomic_val_t)cnt);
	log_store_count_loss(LOG_STORE_STM32, LOG_STORE_LOSS_BACKEND, cnt);
}

static void panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	atomic_set(&panicked, 1);
	log_store_panic();
}

static int is_ready(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	return log_store_ready() ? 0 : -EBUSY;
}

static const struct log_backend_api zephyr_log_source_api = {
	.process = process,
	.dropped = dropped,
	.panic = panic,
	.is_ready = is_ready,
};

LOG_BACKEND_DEFINE(log_backend_log_store, zephyr_log_source_api, true);

void zephyr_log_source_get_stats(struct zephyr_log_source_stats *out)
{
	out->processed = (uint32_t)atomic_get(&stat_processed);
	out->stored = (uint32_t)atomic_get(&stat_stored);
	out->dropped_reported = (uint32_t)atomic_get(&stat_dropped);
	out->store_refused = (uint32_t)atomic_get(&stat_refused);
	out->after_panic = (uint32_t)atomic_get(&stat_after_panic);
	out->truncated = (uint32_t)atomic_get(&stat_truncated);
}

#if defined(CONFIG_ZEPHYR_LOG_SOURCE_INIT_STORE)
static int init_store(void)
{
	if (!log_store_ready()) {
		(void)log_store_init();
	}
	return 0;
}

/* After the log core's own init (CONFIG_LOG_CORE_INIT_PRIORITY), before any
 * thread runs. */
SYS_INIT(init_store, POST_KERNEL, 99);
#endif
