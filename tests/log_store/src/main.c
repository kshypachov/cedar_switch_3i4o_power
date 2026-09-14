/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * log-store: rings, readers, cursors and counters (plan section 12, log-store
 * row). Every test that needs a wrap fills the ring until the store reports
 * overwrites, so the suite runs at 4 KiB and at the firmware's 256 KiB.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <log_store/log_store.h>

#define BUDGET 100000U
#define BOOT_A "boot_0011aabbccddeeff"
#define BOOT_B "boot_ffeeddccbbaa1100"

static struct log_store_record rec;

static int put_full(enum log_store_source source, enum log_store_level level, const char *module,
		    const char *text, uint32_t generation)
{
	const struct log_store_entry e = {
		.source = source,
		.level = level,
		.kind = LOG_STORE_KIND_MESSAGE,
		.generation = generation,
		.uptime_ms = 1000,
		.module = module,
		.module_len = module != NULL ? strlen(module) : 0,
		.text = text,
		.text_len = strlen(text),
	};

	return log_store_append(&e);
}

static void put(enum log_store_source source, enum log_store_level level, const char *module,
		const char *text)
{
	zassert_ok(put_full(source, level, module, text, 1));
}

static void putn(enum log_store_source source, int i)
{
	char text[32];

	snprintf(text, sizeof(text), "msg-%06d", i);
	put(source, LOG_STORE_LEVEL_INFO, "m", text);
}

static struct log_store_filter filter_of(uint8_t sources)
{
	struct log_store_filter f;

	memset(&f, 0, sizeof(f));
	f.sources = sources;
	return f;
}

static const uint64_t zero_pos[LOG_STORE_SOURCE_COUNT];

/* Append numbered records to @p source until at least @p overwrites were pushed out. */
static int fill_until_overwritten(enum log_store_source source, uint64_t overwrites)
{
	struct log_store_stats st;
	int i = 0;

	do {
		putn(source, i++);
		log_store_get_stats(source, &st);
	} while (st.overwritten < overwrites);

	return i;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_ok(log_store_init());
}

ZTEST_SUITE(log_store, NULL, NULL, before, NULL, NULL);

/* -- appending ------------------------------------------------------------- */

ZTEST(log_store, test_record_round_trips_every_field)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	const struct log_store_entry e = {
		.source = LOG_STORE_ESP32,
		.level = LOG_STORE_LEVEL_WARNING,
		.kind = LOG_STORE_KIND_RESET,
		.truncated = true,
		.generation = 7,
		.uptime_ms = 0x123456789AULL,
		.module = "wifi",
		.module_len = 4,
		.text = "hello",
		.text_len = 5,
	};

	zassert_true(log_store_ready());
	zassert_ok(log_store_append(&e));
	log_store_reader_at(&r, &f, zero_pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.source, LOG_STORE_ESP32);
	zassert_equal(rec.level, LOG_STORE_LEVEL_WARNING);
	zassert_equal(rec.kind, LOG_STORE_KIND_RESET);
	zassert_true(rec.truncated);
	zassert_equal(rec.generation, 7);
	zassert_equal(rec.uptime_ms, 0x123456789AULL);
	zassert_equal(rec.seq, 1);
	zassert_true(rec.has_module);
	zassert_str_equal(rec.module, "wifi");
	zassert_equal(rec.text_len, 5);
	zassert_str_equal(rec.text, "hello");
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_END);
}

ZTEST(log_store, test_null_module_and_empty_module_differ)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;

	put(LOG_STORE_STM32, LOG_STORE_LEVEL_NONE, NULL, "printk");
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "", "empty module");
	log_store_reader_at(&r, &f, zero_pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_false(rec.has_module);
	zassert_equal(rec.level, LOG_STORE_LEVEL_NONE);
	zassert_false(rec.truncated);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_true(rec.has_module);
	zassert_str_equal(rec.module, "");
}

ZTEST(log_store, test_invalid_entries_are_refused)
{
	struct log_store_entry e = {
		.source = LOG_STORE_SOURCE_COUNT, .text = "x", .text_len = 1,
	};

	zassert_equal(log_store_append(NULL), -EINVAL);
	zassert_equal(log_store_append(&e), -EINVAL);
	e.source = LOG_STORE_STM32;
	e.level = LOG_STORE_LEVEL_UNKNOWN + 1;
	zassert_equal(log_store_append(&e), -EINVAL);
	e.level = LOG_STORE_LEVEL_INFO;
	e.kind = LOG_STORE_KIND_GAP + 1;
	zassert_equal(log_store_append(&e), -EINVAL);
	e.kind = LOG_STORE_KIND_MESSAGE;
	e.text = NULL;
	zassert_equal(log_store_append(&e), -EINVAL);
	e.text_len = 0;
	zassert_ok(log_store_append(&e), "an empty text is a record");
	zassert_equal(log_store_next_seq(), 2, "refusals take no sequence number");
}

ZTEST(log_store, test_sequence_is_global_across_sources)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	uint64_t expect = 1;

	zassert_equal(log_store_next_seq(), 1);
	for (int i = 0; i < 20; i++) {
		putn((i % 3) == 0 ? LOG_STORE_ESP32 : LOG_STORE_STM32, i);
	}
	zassert_equal(log_store_next_seq(), 21);

	log_store_reader_at(&r, &f, zero_pos);
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		zassert_equal(rec.seq, expect, "source=all merges the rings in seq order");
		zassert_equal(rec.source, ((expect - 1) % 3) == 0 ? LOG_STORE_ESP32 : LOG_STORE_STM32);
		expect++;
	}
	zassert_equal(expect, 21);
	zassert_equal(r.scanned, 20);
}

ZTEST(log_store, test_text_is_cut_at_512_on_a_code_point_boundary)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char text[700];

	/* 511 ASCII + a two-byte character: 513 bytes, cut before the character. */
	memset(text, 'a', 511);
	strcpy(&text[511], "\xC3\xA9");
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, NULL, text);
	/* 510 ASCII + a three-byte character. */
	memset(text, 'b', 510);
	strcpy(&text[510], "\xE2\x82\xAC");
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, NULL, text);
	/* Exactly 512 bytes is not cut. */
	memset(text, 'c', 512);
	text[512] = '\0';
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, NULL, text);
	/* 600 ASCII bytes. */
	memset(text, 'd', 600);
	text[600] = '\0';
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, NULL, text);

	log_store_reader_at(&r, &f, zero_pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.text_len, 511);
	zassert_true(rec.truncated);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.text_len, 510);
	zassert_true(rec.truncated);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.text_len, 512);
	zassert_false(rec.truncated);
	zassert_equal(strlen(rec.text), 512);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.text_len, 512);
	zassert_true(rec.truncated);
}

ZTEST(log_store, test_module_is_cut_at_64)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char module[100];

	memset(module, 'm', 90);
	module[90] = '\0';
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, module, "x");
	log_store_reader_at(&r, &f, zero_pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(strlen(rec.module), LOG_STORE_MODULE_MAX);
	zassert_false(rec.truncated, "truncated is about the text");
}

ZTEST(log_store, test_utf8_cut_edges)
{
	zassert_equal(log_store_utf8_cut("abc", 3, 10), 3);
	zassert_equal(log_store_utf8_cut("abc", 3, 2), 2);
	zassert_equal(log_store_utf8_cut("abc", 3, 0), 0);
	zassert_equal(log_store_utf8_cut("a\xC3\xA9", 3, 2), 1, "inside a 2-byte sequence");
	zassert_equal(log_store_utf8_cut("a\xC3\xA9", 3, 3), 3, "exactly at its end");
	zassert_equal(log_store_utf8_cut("a\xE2\x82\xAC", 4, 3), 1);
	zassert_equal(log_store_utf8_cut("a\xE2\x82\xAC", 4, 2), 1);
	zassert_equal(log_store_utf8_cut("a\xF0\x9F\x98\x80", 5, 4), 1);
	zassert_equal(log_store_utf8_cut("a\xF0\x9F\x98\x80", 5, 5), 5);
	zassert_equal(log_store_utf8_cut("a\xC3", 2, 10), 1, "an unfinished tail is not kept");
	zassert_equal(log_store_utf8_cut("\x80\x80", 2, 10), 2, "stray continuations are left");
	zassert_equal(log_store_utf8_cut("\xC3\xA9z", 3, 3), 3);
}

/* -- wrap and gaps ----------------------------------------------------------- */

ZTEST(log_store, test_wrap_overwrites_the_oldest_and_counts_it)
{
	struct log_store_stats st;
	struct log_store_filter f = filter_of(LOG_STORE_SOURCE_BIT(LOG_STORE_STM32));
	struct log_store_reader r;
	uint64_t expect;
	int n = fill_until_overwritten(LOG_STORE_STM32, 5);

	log_store_get_stats(LOG_STORE_STM32, &st);
	zassert_equal(st.appended, (uint64_t)n);
	zassert_equal(st.appended, st.overwritten + st.records);
	zassert_true(st.used_bytes <= st.capacity_bytes);
	zassert_equal(st.capacity_bytes, CONFIG_LOG_STORE_STM32_KIB * 1024U);
	/* Each numbered record is 28 + 1 + 10 bytes, aligned, plus the trailer. */
	zassert_equal(st.used_bytes, st.records * 44U);

	log_store_reader_at(&r, &f, zero_pos);
	zassert_true(r.gap, "position 0 is behind the tail now");
	expect = st.overwritten + 1;
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		zassert_equal(rec.seq, expect++);
	}
	zassert_equal(expect, (uint64_t)n + 1);

	log_store_get_stats(LOG_STORE_ESP32, &st);
	zassert_equal(st.records, 0, "the other source's ring is untouched");
	zassert_equal(st.overwritten, 0);
}

ZTEST(log_store, test_rings_are_independent)
{
	struct log_store_stats st;

	put(LOG_STORE_ESP32, LOG_STORE_LEVEL_UNKNOWN, NULL, "keep me");
	(void)fill_until_overwritten(LOG_STORE_STM32, 50);
	log_store_get_stats(LOG_STORE_ESP32, &st);
	zassert_equal(st.records, 1, "a flooding STM32 does not push out the ESP32's history");
}

ZTEST(log_store, test_slow_reader_gets_a_gap_and_continues_at_the_tail)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	struct log_store_stats st;
	uint64_t last;

	for (int i = 0; i < 5; i++) {
		putn(LOG_STORE_STM32, i);
	}
	log_store_reader_at(&r, &f, zero_pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.seq, 1);
	zassert_false(r.gap);

	/* The reader sleeps while the ring turns over past its position. */
	(void)fill_until_overwritten(LOG_STORE_STM32, 10);
	log_store_get_stats(LOG_STORE_STM32, &st);

	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_true(r.gap);
	zassert_equal(rec.seq, st.overwritten + 1, "the oldest record still in the ring");
	r.gap = false;
	last = rec.seq;
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		zassert_equal(rec.seq, ++last);
	}
	zassert_false(r.gap);
}

/* -- tail -------------------------------------------------------------------- */

ZTEST(log_store, test_tail_returns_the_newest_oldest_first)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	uint64_t newest;

	(void)fill_until_overwritten(LOG_STORE_STM32, 3);
	newest = log_store_next_seq() - 1;

	log_store_reader_tail(&r, &f, 5, BUDGET);
	zassert_false(r.gap);
	zassert_equal(r.scanned, 5);
	for (uint64_t s = newest - 4; s <= newest; s++) {
		zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
		zassert_equal(rec.seq, s);
	}
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_END);

	/* ...and then what arrives after them. */
	putn(LOG_STORE_ESP32, 1);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.seq, newest + 1);
}

ZTEST(log_store, test_tail_with_a_filter_and_both_sources)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	uint64_t seqs[3];
	int k = 0;

	f.has_module = true;
	strcpy(f.module, "x");
	for (int i = 0; i < 40; i++) {
		char text[16];

		snprintf(text, sizeof(text), "%d", i);
		put((i % 2) ? LOG_STORE_ESP32 : LOG_STORE_STM32, LOG_STORE_LEVEL_INFO,
		    (i % 4) == 1 ? "x" : "y", text);
	}
	/* "x" records are i = 1, 5, ..., 37 → seq i + 1; newest three: 30, 34, 38. */
	log_store_reader_tail(&r, &f, 3, BUDGET);
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		zassert_true(k < 3);
		seqs[k++] = rec.seq;
	}
	zassert_equal(k, 3);
	zassert_equal(seqs[0], 30);
	zassert_equal(seqs[1], 34);
	zassert_equal(seqs[2], 38);
}

ZTEST(log_store, test_tail_merges_both_rings_by_sequence)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	uint64_t expect = 15;

	/* Uneven interleaving: runs of one source then the other. */
	for (int i = 0; i < 20; i++) {
		putn((i / 3) % 2 ? LOG_STORE_ESP32 : LOG_STORE_STM32, i);
	}
	log_store_reader_tail(&r, &f, 6, BUDGET);
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		zassert_equal(rec.seq, expect++);
	}
	zassert_equal(expect, 21);
}

ZTEST(log_store, test_tail_asking_for_more_than_there_is)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	int n = 0;

	for (int i = 0; i < 4; i++) {
		putn(LOG_STORE_ESP32, i);
	}
	log_store_reader_tail(&r, &f, 100, BUDGET);
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		n++;
	}
	zassert_equal(n, 4);
}

ZTEST(log_store, test_tail_of_zero_and_of_an_empty_store)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;

	log_store_reader_tail(&r, &f, 10, BUDGET);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_END);

	putn(LOG_STORE_STM32, 0);
	log_store_reader_tail(&r, &f, 0, BUDGET);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_END, "count 0 is from now");
	putn(LOG_STORE_STM32, 1);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.seq, 2);
}

ZTEST(log_store, test_tail_is_bounded_by_the_scan_budget)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;

	f.has_module = true;
	strcpy(f.module, "wanted");
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "wanted", "old match");
	for (int i = 0; i < 50; i++) {
		putn(LOG_STORE_STM32, i);
	}
	log_store_reader_tail(&r, &f, 5, 10);
	zassert_equal(r.scanned, 10, "no more than the budget");
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_END,
		      "the match beyond the budget is not reached");
}

/* -- filters ----------------------------------------------------------------- */

ZTEST(log_store, test_min_level_keeps_null_and_unknown)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char seen[16] = "";

	put(LOG_STORE_STM32, LOG_STORE_LEVEL_DEBUG, NULL, "d");
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, NULL, "i");
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_WARNING, NULL, "w");
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_ERROR, NULL, "e");
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_NONE, NULL, "n");
	put(LOG_STORE_ESP32, LOG_STORE_LEVEL_UNKNOWN, NULL, "u");

	f.min_level = LOG_STORE_LEVEL_WARNING;
	log_store_reader_at(&r, &f, zero_pos);
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		strcat(seen, rec.text);
	}
	zassert_str_equal(seen, "wenu");
}

ZTEST(log_store, test_source_module_and_contains_filters)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCE_BIT(LOG_STORE_ESP32));
	struct log_store_reader r;
	char seen[32] = "";

	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "wifi", "A DHCP lease");
	put(LOG_STORE_ESP32, LOG_STORE_LEVEL_INFO, "wifi", "B dhcp done");
	put(LOG_STORE_ESP32, LOG_STORE_LEVEL_INFO, "wifi2", "C DhCp prefix module");
	put(LOG_STORE_ESP32, LOG_STORE_LEVEL_INFO, NULL, "D dhcp no module");
	put(LOG_STORE_ESP32, LOG_STORE_LEVEL_INFO, "wifi", "E other");

	f.has_module = true;
	strcpy(f.module, "wifi");
	f.has_contains = true;
	strcpy(f.contains, "DHCP");
	log_store_reader_at(&r, &f, zero_pos);
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		seen[strlen(seen)] = rec.text[0];
	}
	zassert_str_equal(seen, "B", "ESP32 only, module exact, text case-insensitive");
	zassert_equal(r.scanned, 4, "the STM32 ring is not walked for source=esp32");
}

ZTEST(log_store, test_filter_match_directly)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCE_BIT(LOG_STORE_STM32));
	struct log_store_record x = {
		.source = LOG_STORE_STM32,
		.level = LOG_STORE_LEVEL_DEBUG,
		.has_module = true,
		.module = "net",
		.text_len = 11,
		.text = "Hello World",
	};

	zassert_true(log_store_filter_match(&f, &x));
	f.has_contains = true;
	strcpy(f.contains, "o wOR");
	zassert_true(log_store_filter_match(&f, &x));
	strcpy(f.contains, "");
	zassert_true(log_store_filter_match(&f, &x), "an empty substring is everywhere");
	strcpy(f.contains, "World!");
	zassert_false(log_store_filter_match(&f, &x));
	f.has_contains = false;
	f.min_level = LOG_STORE_LEVEL_INFO;
	zassert_false(log_store_filter_match(&f, &x));
	f.min_level = LOG_STORE_LEVEL_DEBUG;
	zassert_true(log_store_filter_match(&f, &x));
	f.has_module = true;
	strcpy(f.module, "ne");
	zassert_false(log_store_filter_match(&f, &x), "module is exact, not a prefix");
	x.has_module = false;
	strcpy(f.module, "");
	zassert_false(log_store_filter_match(&f, &x), "a null module matches no module filter");
	f.has_module = false;
	x.source = LOG_STORE_ESP32;
	zassert_false(log_store_filter_match(&f, &x));
}

ZTEST(log_store, test_no_sources_matches_nothing_and_stands_at_the_heads)
{
	struct log_store_filter none = filter_of(0);
	struct log_store_filter all = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char cursor[LOG_STORE_CURSOR_MAX];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];

	for (int i = 0; i < 6; i++) {
		putn(i % 2 ? LOG_STORE_ESP32 : LOG_STORE_STM32, i);
	}
	log_store_reader_tail(&r, &none, 10, BUDGET);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_END);
	log_store_reader_at(&r, &none, zero_pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_END);
	zassert_true(log_store_cursor_encode(&r, 1, cursor, sizeof(cursor)) > 0);
	zassert_ok(log_store_cursor_decode(cursor, 1, &none, pos));

	/* The positions mean "from now" for any later reader too. */
	putn(LOG_STORE_STM32, 99);
	log_store_reader_at(&r, &all, pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.seq, 7);
}

/* -- scan budget ------------------------------------------------------------- */

ZTEST(log_store, test_scan_budget_continues_without_rescanning)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char cursor[LOG_STORE_CURSOR_MAX];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	uint32_t tag = log_store_boot_tag(BOOT_A);
	int budgets = 0;

	f.has_module = true;
	strcpy(f.module, "b");
	for (int i = 0; i < 30; i++) {
		put(i % 2 ? LOG_STORE_ESP32 : LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "a", "skip");
	}
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "b", "found");

	log_store_reader_at(&r, &f, zero_pos);
	for (;;) {
		enum log_store_read_result res = log_store_read(&r, &rec, 10);

		if (res != LOG_STORE_READ_BUDGET) {
			zassert_equal(res, LOG_STORE_READ_RECORD);
			break;
		}
		budgets++;
		/* Resume through a cursor, as the next HTTP request would. */
		zassert_true(log_store_cursor_encode(&r, tag, cursor, sizeof(cursor)) > 0);
		zassert_ok(log_store_cursor_decode(cursor, tag, &f, pos));
		log_store_reader_at(&r, &f, pos);
	}
	zassert_equal(budgets, 3);
	zassert_equal(rec.seq, 31);
	zassert_str_equal(rec.text, "found");
	zassert_equal(r.scanned, 1, "after the third resume only the match was looked at");
	zassert_equal(log_store_read(&r, &rec, 10), LOG_STORE_READ_END);
}

ZTEST(log_store, test_zero_budget_looks_at_nothing)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;

	putn(LOG_STORE_STM32, 0);
	log_store_reader_at(&r, &f, zero_pos);
	zassert_equal(log_store_read(&r, &rec, 0), LOG_STORE_READ_BUDGET);
	zassert_equal(r.scanned, 0);
	zassert_equal(log_store_read(&r, &rec, 1), LOG_STORE_READ_RECORD);
}

/* -- export snapshot ------------------------------------------------------- */

ZTEST(log_store, test_upper_seq_bounds_a_snapshot)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	uint64_t upper;
	int n = 0;

	for (int i = 0; i < 10; i++) {
		putn(i % 2 ? LOG_STORE_ESP32 : LOG_STORE_STM32, i);
	}
	log_store_reader_tail(&r, &f, 2000, BUDGET);
	upper = log_store_next_seq();
	r.upper_seq = upper;
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	n++;
	/* Producers keep writing during the export. */
	for (int i = 0; i < 5; i++) {
		putn(i % 2 ? LOG_STORE_ESP32 : LOG_STORE_STM32, 100 + i);
	}
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		zassert_true(rec.seq < upper);
		n++;
	}
	zassert_equal(n, 10, "exactly the records that existed when the export started");
}

/* -- cursors ------------------------------------------------------------------ */

ZTEST(log_store, test_cursor_round_trip)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char cursor[LOG_STORE_CURSOR_MAX];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	uint32_t tag = log_store_boot_tag(BOOT_A);
	int len;

	f.min_level = LOG_STORE_LEVEL_INFO;
	f.has_contains = true;
	strcpy(f.contains, "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
	for (int i = 0; i < 7; i++) {
		putn(i % 2 ? LOG_STORE_ESP32 : LOG_STORE_STM32, i);
	}
	struct log_store_filter all = filter_of(LOG_STORE_SOURCES_ALL);

	log_store_reader_tail(&r, &all, 3, BUDGET);
	r.filter = f;
	len = log_store_cursor_encode(&r, tag, cursor, sizeof(cursor));
	zassert_true(len > 0 && len < LOG_STORE_CURSOR_MAX);
	zassert_equal(strlen(cursor), (size_t)len);
	for (int i = 0; i < len; i++) {
		zassert_not_null(strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_",
					cursor[i]), "base64url, unpadded");
	}
	zassert_ok(log_store_cursor_decode(cursor, tag, &f, pos));
	zassert_equal(pos[0], r.pos[0]);
	zassert_equal(pos[1], r.pos[1]);
	zassert_equal(log_store_cursor_encode(&r, tag, cursor, (size_t)len), -ENOSPC);
}

ZTEST(log_store, test_cursor_bound_to_filters_and_boot)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_filter g;
	struct log_store_reader r;
	char cursor[LOG_STORE_CURSOR_MAX];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	uint32_t tag_a = log_store_boot_tag(BOOT_A);
	uint32_t tag_b = log_store_boot_tag(BOOT_B);

	zassert_not_equal(tag_a, tag_b);
	putn(LOG_STORE_STM32, 0);
	log_store_reader_tail(&r, &f, 1, BUDGET);
	zassert_true(log_store_cursor_encode(&r, tag_a, cursor, sizeof(cursor)) > 0);

	zassert_equal(log_store_cursor_decode(cursor, tag_b, &f, pos), -ESTALE,
		      "another boot: the caller answers with the tail and gap");

	g = filter_of(LOG_STORE_SOURCE_BIT(LOG_STORE_ESP32));
	zassert_equal(log_store_cursor_decode(cursor, tag_a, &g, pos), -EINVAL);
	g = f;
	g.min_level = LOG_STORE_LEVEL_ERROR;
	zassert_equal(log_store_cursor_decode(cursor, tag_a, &g, pos), -EINVAL);
	g = f;
	g.has_module = true;
	zassert_equal(log_store_cursor_decode(cursor, tag_a, &g, pos), -EINVAL,
		      "an empty module filter is still a filter");
	g = f;
	g.has_contains = true;
	strcpy(g.contains, "x");
	zassert_equal(log_store_cursor_decode(cursor, tag_a, &g, pos), -EINVAL);
	g = f;
	zassert_equal(log_store_cursor_decode(cursor, tag_b, &g, pos), -ESTALE);
}

ZTEST(log_store, test_corrupt_and_foreign_cursors_are_invalid)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char cursor[LOG_STORE_CURSOR_MAX];
	char bad[LOG_STORE_CURSOR_MAX + 8];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	uint32_t tag = log_store_boot_tag(BOOT_A);
	int len;

	putn(LOG_STORE_STM32, 0);
	log_store_reader_tail(&r, &f, 1, BUDGET);
	len = log_store_cursor_encode(&r, tag, cursor, sizeof(cursor));

	zassert_equal(log_store_cursor_decode("", tag, &f, pos), -EINVAL);
	zassert_equal(log_store_cursor_decode(NULL, tag, &f, pos), -EINVAL);
	zassert_equal(log_store_cursor_decode("0", tag, &f, pos), -EINVAL);
	zassert_equal(log_store_cursor_decode("not-a-real-cursor", tag, &f, pos), -EINVAL);

	for (int i = 0; i < len; i++) {
		strcpy(bad, cursor);
		bad[i] = (bad[i] == 'A') ? 'B' : 'A';
		zassert_equal(log_store_cursor_decode(bad, tag, &f, pos), -EINVAL,
			      "one character changed at %d", i);
	}
	strcpy(bad, cursor);
	bad[3] = '+';
	zassert_equal(log_store_cursor_decode(bad, tag, &f, pos), -EINVAL, "not base64url");
	strcpy(bad, cursor);
	strcat(bad, "A");
	zassert_equal(log_store_cursor_decode(bad, tag, &f, pos), -EINVAL, "too long");
	bad[len - 1] = '\0';
	zassert_equal(log_store_cursor_decode(bad, tag, &f, pos), -EINVAL, "too short");
}

ZTEST(log_store, test_cursor_beyond_the_head_is_invalid)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char cursor[LOG_STORE_CURSOR_MAX];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	uint32_t tag = log_store_boot_tag(BOOT_A);

	for (int i = 0; i < 4; i++) {
		putn(LOG_STORE_ESP32, i);
	}
	log_store_reader_tail(&r, &f, 0, BUDGET);
	zassert_true(log_store_cursor_encode(&r, tag, cursor, sizeof(cursor)) > 0);
	zassert_ok(log_store_cursor_decode(cursor, tag, &f, pos), "exactly at the head is fine");

	r.pos[LOG_STORE_ESP32] += 44;
	zassert_true(log_store_cursor_encode(&r, tag, cursor, sizeof(cursor)) > 0);
	zassert_equal(log_store_cursor_decode(cursor, tag, &f, pos), -EINVAL,
		      "a position this store never wrote");
	r.pos[LOG_STORE_ESP32] -= 45;
	zassert_true(log_store_cursor_encode(&r, tag, cursor, sizeof(cursor)) > 0);
	zassert_equal(log_store_cursor_decode(cursor, tag, &f, pos), -EINVAL,
		      "a position between records");

	/* Same boot, but the store was emptied (only tests do this). */
	r.pos[LOG_STORE_ESP32] += 1;
	zassert_true(log_store_cursor_encode(&r, tag, cursor, sizeof(cursor)) > 0);
	zassert_ok(log_store_init());
	zassert_equal(log_store_cursor_decode(cursor, tag, &f, pos), -EINVAL);
}

ZTEST(log_store, test_cursor_behind_the_tail_decodes_and_reports_a_gap)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	struct log_store_stats st;
	char cursor[LOG_STORE_CURSOR_MAX];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	uint32_t tag = log_store_boot_tag(BOOT_A);

	putn(LOG_STORE_STM32, 0);
	log_store_reader_at(&r, &f, zero_pos);
	zassert_true(log_store_cursor_encode(&r, tag, cursor, sizeof(cursor)) > 0);
	(void)fill_until_overwritten(LOG_STORE_STM32, 3);

	zassert_ok(log_store_cursor_decode(cursor, tag, &f, pos));
	log_store_reader_at(&r, &f, pos);
	zassert_true(r.gap);
	log_store_get_stats(LOG_STORE_STM32, &st);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.seq, st.overwritten + 1);
}

ZTEST(log_store, test_gap_only_for_selected_sources)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCE_BIT(LOG_STORE_ESP32));
	struct log_store_reader r;

	(void)fill_until_overwritten(LOG_STORE_STM32, 3);
	log_store_reader_at(&r, &f, zero_pos);
	zassert_false(r.gap, "the STM32 ring's wrap is nothing to an esp32 reader");
}

/* -- concurrency ---------------------------------------------------------------- */

#define WRITER_RECORDS 3000

static K_THREAD_STACK_DEFINE(writer_stack, 2048);
static struct k_thread writer_thread;
static atomic_t writer_done;

static void writer(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (int i = 0; i < WRITER_RECORDS; i++) {
		putn(i % 5 ? LOG_STORE_STM32 : LOG_STORE_ESP32, i);
		if ((i % 13) == 0) {
			/* Let simulated time pass so the reader interleaves. */
			k_sleep(K_MSEC(1));
		}
	}
	atomic_set(&writer_done, 1);
}

ZTEST(log_store, test_producer_during_reads)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	uint64_t last = 0;
	uint64_t got = 0;
	uint64_t skipped = 0;
	uint32_t gaps = 0;
	/*
	 * `gap` is a reader-level flag: "records were overwritten since you last
	 * looked". With two rings the STM32 ring's loss can be noticed while the
	 * next record returned is an ESP32 one with the adjacent sequence, and the
	 * skipped numbers show up a few records later. So the invariant checked
	 * is: every skip is preceded by a gap reported since the reader last
	 * caught up (READ_END), not necessarily on the same record.
	 */
	bool gap_pending = false;

	atomic_set(&writer_done, 0);
	log_store_reader_at(&r, &f, zero_pos);
	k_thread_create(&writer_thread, writer_stack, K_THREAD_STACK_SIZEOF(writer_stack), writer,
			NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);

	for (;;) {
		enum log_store_read_result res = log_store_read(&r, &rec, 3);

		if (r.gap) {
			gaps++;
			gap_pending = true;
			r.gap = false;
		}
		if (res == LOG_STORE_READ_RECORD) {
			zassert_true(rec.seq > last, "never out of order, never twice");
			if (rec.seq != last + 1) {
				zassert_true(gap_pending, "a skipped sequence is always reported");
				skipped += rec.seq - last - 1;
			}
			last = rec.seq;
			got++;
			if ((got % 11) == 0) {
				k_sleep(K_MSEC(1)); /* a slow reader */
			}
		} else if (res == LOG_STORE_READ_END) {
			zassert_equal(r.gap, false);
			gap_pending = false;
			if (atomic_get(&writer_done) != 0) {
				break;
			}
			k_sleep(K_MSEC(1));
		}
	}
	k_thread_join(&writer_thread, K_FOREVER);
	/* The writer may have finished after the reader's last look. */
	while (log_store_read(&r, &rec, BUDGET) == LOG_STORE_READ_RECORD) {
		if (r.gap) {
			gap_pending = true;
			r.gap = false;
		}
		zassert_true(rec.seq > last);
		if (rec.seq != last + 1) {
			zassert_true(gap_pending);
			skipped += rec.seq - last - 1;
		}
		last = rec.seq;
		got++;
	}

	zassert_equal(last, WRITER_RECORDS);
	zassert_equal(got + skipped, WRITER_RECORDS);
	TC_PRINT("concurrent: %llu read, %llu overwritten before read, %u gaps\n",
		 (unsigned long long)got, (unsigned long long)skipped, gaps);
}

/* -- panic, losses, stats, names ------------------------------------------ */

ZTEST(log_store, test_panic_refuses_appends)
{
	struct log_store_stats st;

	putn(LOG_STORE_STM32, 0);
	log_store_panic();
	zassert_equal(put_full(LOG_STORE_STM32, LOG_STORE_LEVEL_ERROR, NULL, "fault", 1), -EAGAIN);
	log_store_get_stats(LOG_STORE_STM32, &st);
	zassert_equal(st.records, 1, "what was stored stays readable");
	zassert_ok(log_store_init());
	zassert_ok(put_full(LOG_STORE_STM32, LOG_STORE_LEVEL_ERROR, NULL, "again", 1));
}

ZTEST(log_store, test_losses_are_counted_per_source_and_kind)
{
	struct log_store_stats st;

	log_store_count_loss(LOG_STORE_STM32, LOG_STORE_LOSS_BACKEND, 3);
	log_store_count_loss(LOG_STORE_ESP32, LOG_STORE_LOSS_UART, 5);
	log_store_count_loss(LOG_STORE_ESP32, LOG_STORE_LOSS_BACKEND, 1);
	log_store_count_loss(LOG_STORE_SOURCE_COUNT, LOG_STORE_LOSS_UART, 100);
	log_store_count_loss(LOG_STORE_STM32, LOG_STORE_LOSS_COUNT, 100);

	zassert_equal(log_store_dropped(LOG_STORE_SOURCE_BIT(LOG_STORE_STM32)), 3);
	zassert_equal(log_store_dropped(LOG_STORE_SOURCE_BIT(LOG_STORE_ESP32)), 6);
	zassert_equal(log_store_dropped(LOG_STORE_SOURCES_ALL), 9);
	zassert_equal(log_store_dropped(0), 0);

	(void)fill_until_overwritten(LOG_STORE_STM32, 2);
	zassert_equal(log_store_dropped(LOG_STORE_SOURCES_ALL), 9,
		      "overwrites are not losses before the ring; gap reports them");

	log_store_get_stats(LOG_STORE_ESP32, &st);
	zassert_equal(st.lost[LOG_STORE_LOSS_UART], 5);
	zassert_equal(st.lost[LOG_STORE_LOSS_BACKEND], 1);

	zassert_ok(log_store_init());
	zassert_equal(log_store_dropped(LOG_STORE_SOURCES_ALL), 0);
}

ZTEST(log_store, test_stats_and_timing)
{
	struct log_store_stats st;
	struct log_store_timing t;
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;

	log_store_get_stats(LOG_STORE_SOURCE_COUNT, &st);
	zassert_equal(st.capacity_bytes, 0);
	log_store_get_stats(LOG_STORE_ESP32, &st);
	zassert_equal(st.records, 0);
	zassert_equal(st.used_bytes, 0);
	zassert_equal(st.capacity_bytes, CONFIG_LOG_STORE_ESP32_KIB * 1024U);

	put(LOG_STORE_ESP32, LOG_STORE_LEVEL_UNKNOWN, NULL, "12345");
	log_store_get_stats(LOG_STORE_ESP32, &st);
	zassert_equal(st.records, 1);
	zassert_equal(st.appended, 1);
	zassert_equal(st.used_bytes, 40, "header 28 + text 5, aligned to 36, + trailer 4");

	log_store_reader_at(&r, &f, zero_pos);
	(void)log_store_read(&r, &rec, BUDGET);
	log_store_get_timing(&t);
	TC_PRINT("lock: append hold %u us, wait %u us, read hold %u us\n", t.append_hold_max_us,
		 t.append_wait_max_us, t.read_hold_max_us);
}

ZTEST(log_store, test_wire_names)
{
	zassert_str_equal(log_store_source_str(LOG_STORE_STM32), "stm32");
	zassert_str_equal(log_store_source_str(LOG_STORE_ESP32), "esp32");
	zassert_is_null(log_store_source_str(LOG_STORE_SOURCE_COUNT));
	zassert_is_null(log_store_level_str(LOG_STORE_LEVEL_NONE));
	zassert_str_equal(log_store_level_str(LOG_STORE_LEVEL_DEBUG), "debug");
	zassert_str_equal(log_store_level_str(LOG_STORE_LEVEL_INFO), "info");
	zassert_str_equal(log_store_level_str(LOG_STORE_LEVEL_WARNING), "warning");
	zassert_str_equal(log_store_level_str(LOG_STORE_LEVEL_ERROR), "error");
	zassert_str_equal(log_store_level_str(LOG_STORE_LEVEL_UNKNOWN), "unknown");
	zassert_str_equal(log_store_kind_str(LOG_STORE_KIND_MESSAGE), "message");
	zassert_str_equal(log_store_kind_str(LOG_STORE_KIND_RESET), "reset");
	zassert_str_equal(log_store_kind_str(LOG_STORE_KIND_PAUSED), "paused");
	zassert_str_equal(log_store_kind_str(LOG_STORE_KIND_GAP), "gap");
	zassert_is_null(log_store_kind_str(LOG_STORE_KIND_GAP + 1));
	zassert_not_equal(log_store_boot_tag(BOOT_A), log_store_boot_tag(NULL));
}

/* -- sizing, for the README ---------------------------------------------------- */

ZTEST(log_store, test_records_per_ring_for_typical_lines)
{
	static const char *const lines[] = {
		"Received: 192.168.88.13",
		"IPv6 address added, starting Matter",
		"W5500 frame length 65533 exceeds 65535 buffered bytes, dropped",
		"invalid header: 0xffffffff",
	};
	struct log_store_stats st;
	size_t text_bytes = 0;
	int i = 0;

	do {
		const char *t = lines[i % ARRAY_SIZE(lines)];

		put(LOG_STORE_ESP32, LOG_STORE_LEVEL_INFO, "net_dhcpv4", t);
		if (i < (int)ARRAY_SIZE(lines)) {
			text_bytes += strlen(t);
		}
		i++;
		log_store_get_stats(LOG_STORE_ESP32, &st);
	} while (st.overwritten == 0);

	TC_PRINT("ring %u KiB holds %u records of ~%zu-byte text (%u bytes each on average)\n",
		 st.capacity_bytes / 1024U, st.records, text_bytes / ARRAY_SIZE(lines),
		 st.used_bytes / st.records);
	zassert_true(st.records > 0);
}

/* -- found by mutation (reports/p5/notes-mutations-logs.md) ------------------- */

/* A marker with no module and no text is the smallest record there is (header
 * and trailer only): it must read back forwards and be walked over backwards. */
ZTEST(log_store, test_the_smallest_record_reads_back_both_ways)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	const struct log_store_entry empty = {
		.source = LOG_STORE_STM32,
		.kind = LOG_STORE_KIND_PAUSED,
	};

	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "m", "before");
	zassert_ok(log_store_append(&empty));
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "m", "after");

	log_store_reader_at(&r, &f, zero_pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_str_equal(rec.text, "before");
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.kind, LOG_STORE_KIND_PAUSED);
	zassert_equal(rec.text_len, 0);
	zassert_false(rec.has_module);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_str_equal(rec.text, "after");

	log_store_reader_tail(&r, &f, 3, BUDGET);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_str_equal(rec.text, "before", "the backward walk crossed the smallest record");
}

/* A module longer than 64 bytes is stored cut, and the text after it is intact:
 * the cut is made before the record's layout, not only when it is read. */
ZTEST(log_store, test_a_long_module_is_cut_before_the_text_is_placed)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char module[301];
	char wide[70];

	memset(module, 'm', 300);
	module[300] = '\0';
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, module, "payload one");

	/* 63 ASCII bytes and a two-byte letter across the 64-byte limit. */
	memset(wide, 'w', 63);
	memcpy(&wide[63], "\xc3\xa9", 3);
	put(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, wide, "payload two");

	log_store_reader_at(&r, &f, zero_pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(strlen(rec.module), LOG_STORE_MODULE_MAX);
	zassert_str_equal(rec.text, "payload one");
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(strlen(rec.module), 63, "cut before the letter");
	zassert_str_equal(rec.text, "payload two");
}

/* A ring holds exactly its size: the record that fills the last byte does not
 * push anything out, the next one does. */
ZTEST(log_store, test_a_ring_filled_to_the_last_byte_overwrites_nothing)
{
	const struct log_store_entry smallest = {.source = LOG_STORE_ESP32};
	struct log_store_stats st;
	uint32_t fits;

	log_store_get_stats(LOG_STORE_ESP32, &st);
	/* The smallest record: a 28-byte header and a 4-byte trailer. */
	fits = st.capacity_bytes / 32U;
	for (uint32_t i = 0; i < fits; i++) {
		zassert_ok(log_store_append(&smallest));
	}
	log_store_get_stats(LOG_STORE_ESP32, &st);
	zassert_equal(st.used_bytes, st.capacity_bytes);
	zassert_equal(st.records, fits);
	zassert_equal(st.overwritten, 0);

	zassert_ok(log_store_append(&smallest));
	log_store_get_stats(LOG_STORE_ESP32, &st);
	zassert_equal(st.records, fits);
	zassert_equal(st.overwritten, 1);
}

/* The last valid level and kind are values, not bounds. */
ZTEST(log_store, test_the_last_level_and_kind_are_accepted)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	const struct log_store_entry e = {
		.source = LOG_STORE_ESP32,
		.level = LOG_STORE_LEVEL_UNKNOWN,
		.kind = LOG_STORE_KIND_GAP,
		.text = "g",
		.text_len = 1,
	};

	zassert_ok(log_store_append(&e));
	log_store_reader_at(&r, &f, zero_pos);
	zassert_equal(log_store_read(&r, &rec, BUDGET), LOG_STORE_READ_RECORD);
	zassert_equal(rec.level, LOG_STORE_LEVEL_UNKNOWN);
	zassert_equal(rec.kind, LOG_STORE_KIND_GAP);
}

/* A reader placed past a head stands at the head, so the cursor it gives is one
 * the store accepts. */
ZTEST(log_store, test_a_reader_placed_past_the_head_stands_at_it)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	struct log_store_stats st;
	char cursor[LOG_STORE_CURSOR_MAX];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	const uint64_t far[LOG_STORE_SOURCE_COUNT] = {1U << 20, 1U << 20};

	putn(LOG_STORE_STM32, 0);
	log_store_reader_at(&r, &f, far);
	log_store_get_stats(LOG_STORE_STM32, &st);
	zassert_equal(r.pos[LOG_STORE_STM32], st.used_bytes, "the STM32 head");
	zassert_equal(r.pos[LOG_STORE_ESP32], 0, "the empty ESP32 ring's head");
	zassert_false(r.gap);
	zassert_true(log_store_cursor_encode(&r, 1, cursor, sizeof(cursor)) > 0);
	zassert_ok(log_store_cursor_decode(cursor, 1, &f, pos));
}

/* A cursor has one spelling: its last character carries two unused bits, and a
 * cursor with them set is not one this store issued, even though the bytes it
 * decodes to are the same. */
ZTEST(log_store, test_a_cursor_with_its_unused_bits_set_is_invalid)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_reader r;
	char cursor[LOG_STORE_CURSOR_MAX];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	int len;
	int value;

	putn(LOG_STORE_STM32, 0);
	log_store_reader_tail(&r, &f, 1, BUDGET);
	len = log_store_cursor_encode(&r, 1, cursor, sizeof(cursor));
	zassert_true(len > 0);
	zassert_ok(log_store_cursor_decode(cursor, 1, &f, pos));

	value = (int)(strchr(alphabet, cursor[len - 1]) - alphabet);
	zassert_equal(value & 3, 0, "canonical: the unused bits are zero");
	cursor[len - 1] = alphabet[value | 1];
	zassert_equal(log_store_cursor_decode(cursor, 1, &f, pos), -EINVAL);
}

/* A cursor is bound to the filter's text, not only to its length: another
 * substring or module of the same length is another filter. */
ZTEST(log_store, test_a_cursor_is_bound_to_the_filter_text)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCES_ALL);
	struct log_store_filter g;
	struct log_store_reader r;
	char cursor[LOG_STORE_CURSOR_MAX];
	uint64_t pos[LOG_STORE_SOURCE_COUNT];

	putn(LOG_STORE_STM32, 0);
	f.has_contains = true;
	strcpy(f.contains, "abc");
	f.has_module = true;
	strcpy(f.module, "net");
	log_store_reader_tail(&r, &f, 1, BUDGET);
	zassert_true(log_store_cursor_encode(&r, 1, cursor, sizeof(cursor)) > 0);
	zassert_ok(log_store_cursor_decode(cursor, 1, &f, pos));

	g = f;
	strcpy(g.contains, "abd");
	zassert_equal(log_store_cursor_decode(cursor, 1, &g, pos), -EINVAL, "other substring");
	g = f;
	strcpy(g.module, "nfs");
	zassert_equal(log_store_cursor_decode(cursor, 1, &g, pos), -EINVAL, "other module");
}

/* A substring that ends where the text ends, or is the whole text, is found. */
ZTEST(log_store, test_contains_at_the_end_and_the_whole_text)
{
	struct log_store_filter f = filter_of(LOG_STORE_SOURCE_BIT(LOG_STORE_STM32));
	struct log_store_record x = {
		.source = LOG_STORE_STM32,
		.level = LOG_STORE_LEVEL_INFO,
		.text_len = 11,
		.text = "Hello World",
	};

	f.has_contains = true;
	strcpy(f.contains, "WORLD");
	zassert_true(log_store_filter_match(&f, &x), "at the end");
	strcpy(f.contains, "hello world");
	zassert_true(log_store_filter_match(&f, &x), "the whole text");
	strcpy(f.contains, "hello world!");
	zassert_false(log_store_filter_match(&f, &x), "longer than the text");
}
