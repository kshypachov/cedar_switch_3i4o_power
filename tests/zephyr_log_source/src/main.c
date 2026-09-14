/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr log backend over the real deferred log core and the real
 * log-store. Messages are processed when the test calls log_process(), so a
 * flood can overflow the core's buffer on purpose.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/ztest.h>

#include <log_store/log_store.h>
#include <zephyr_log_source/zephyr_log_source.h>

LOG_MODULE_REGISTER(zls_test, LOG_LEVEL_DBG);

#define FFFD "\xEF\xBF\xBD"
#define STM32_ONLY LOG_STORE_SOURCE_BIT(LOG_STORE_STM32)

static const struct log_backend *backend;
static bool active_at_boot;

static struct log_store_record records[64];
static size_t record_count;

static void drain(void)
{
	while (log_process()) {
	}
}

/* The newest records of the STM32 ring, oldest first. */
static size_t read_records(void)
{
	struct log_store_filter filter = {.sources = STM32_ONLY};
	struct log_store_reader reader;

	log_store_reader_tail(&reader, &filter, ARRAY_SIZE(records), 100000);
	record_count = 0;
	while (record_count < ARRAY_SIZE(records) &&
	       log_store_read(&reader, &records[record_count], 100000) == LOG_STORE_READ_RECORD) {
		record_count++;
	}
	return record_count;
}

static uint64_t appended(void)
{
	struct log_store_stats stats;

	log_store_get_stats(LOG_STORE_STM32, &stats);
	return stats.appended;
}

static void *setup(void)
{
	backend = log_backend_get_by_name("log_backend_log_store");
	zassert_not_null(backend);
	/* The log core initialised its backends at POST_KERNEL priority 0, before
	 * the store (priority 99), so this backend was left for the processing
	 * thread to activate - and there is no thread in this configuration. */
	active_at_boot = log_backend_is_active(backend);
	zassert_true(log_store_ready(), "CONFIG_ZEPHYR_LOG_SOURCE_INIT_STORE initialised it");
	if (!active_at_boot) {
		log_backend_enable(backend, backend->cb->ctx, LOG_LEVEL_DBG);
	}
	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	drain();
	(void)log_store_init();
}

ZTEST_SUITE(zephyr_log_source, NULL, setup, before, NULL, NULL);

ZTEST(zephyr_log_source, test_a_backend_is_not_ready_before_the_store)
{
	zassert_false(active_at_boot,
		      "is_ready() must have answered -EBUSY while the store was not initialised");
	zassert_equal(log_backend_is_ready(backend), 0, "ready once the store is");
}

ZTEST(zephyr_log_source, test_the_text_has_no_prefix_and_levels_and_module_are_fields)
{
	int64_t before_ms = k_uptime_get();

	LOG_ERR("error %d", 1);
	LOG_WRN("warning");
	LOG_INF("info %s", "text");
	LOG_DBG("debug");
	k_msleep(5);
	drain();

	zassert_equal(read_records(), 4);
	zassert_str_equal(records[0].text, "error 1");
	zassert_equal(records[0].level, LOG_STORE_LEVEL_ERROR);
	zassert_str_equal(records[1].text, "warning");
	zassert_equal(records[1].level, LOG_STORE_LEVEL_WARNING);
	zassert_str_equal(records[2].text, "info text");
	zassert_equal(records[2].level, LOG_STORE_LEVEL_INFO);
	/* CONFIG_LOG_FUNC_NAME_PREFIX_DBG: the core puts the function in the format. */
	zassert_equal(records[3].level, LOG_STORE_LEVEL_DEBUG);
	zassert_not_null(strstr(records[3].text, "test_the_text_has_no_prefix"), "%s",
			 records[3].text);
	zassert_true(records[3].text_len >= 7 &&
			     strcmp(&records[3].text[records[3].text_len - 7], ": debug") == 0,
		     "%s", records[3].text);

	for (size_t i = 0; i < record_count; i++) {
		zassert_true(records[i].has_module);
		zassert_str_equal(records[i].module, "zls_test");
		zassert_equal(records[i].source, LOG_STORE_STM32);
		zassert_equal(records[i].kind, LOG_STORE_KIND_MESSAGE);
		zassert_equal(records[i].generation, 0);
		zassert_false(records[i].truncated);
		zassert_is_null(strchr(records[i].text, '\n'), "no line end: %s", records[i].text);
		/* The time the message was logged, not the time it was processed. */
		zassert_true(records[i].uptime_ms >= (uint64_t)before_ms &&
				     records[i].uptime_ms <= (uint64_t)before_ms + 2,
			     "uptime %llu, logged at %lld", records[i].uptime_ms, before_ms);
		if (i > 0) {
			zassert_true(records[i].seq > records[i - 1].seq);
		}
	}
}

ZTEST(zephyr_log_source, test_a_printk_style_message_has_no_level_and_no_module)
{
	LOG_PRINTK("raw %d\n", 7);
	LOG_RAW("raw without cr %s\n", "too");
	drain();

	zassert_equal(read_records(), 2);
	for (size_t i = 0; i < 2; i++) {
		zassert_equal(records[i].level, LOG_STORE_LEVEL_NONE);
		zassert_false(records[i].has_module);
	}
	zassert_str_equal(records[0].text, "raw 7");
	zassert_str_equal(records[1].text, "raw without cr too");
}

ZTEST(zephyr_log_source, test_a_hexdump_keeps_its_lines)
{
	static const uint8_t blob[20] = {0x01, 0x02, 0x03, 0x41, 0x42};

	LOG_HEXDUMP_INF(blob, sizeof(blob), "blob");
	drain();

	zassert_equal(read_records(), 1);
	zassert_equal(records[0].level, LOG_STORE_LEVEL_INFO);
	zassert_true(strncmp(records[0].text, "blob", 4) == 0, "%s", records[0].text);
	zassert_not_null(strstr(records[0].text, "\n"), "%s", records[0].text);
	zassert_not_null(strstr(records[0].text, "01 02 03 41 42"), "%s", records[0].text);
	zassert_not_equal(records[0].text[records[0].text_len - 1], '\n');
}

ZTEST(zephyr_log_source, test_control_characters_and_bad_utf8_are_replaced)
{
	LOG_INF("a\tb\r\nc\x01\x02" "d\x1b[0m \xff\xfe \xd0\x9f\xd1\x80\xd0\xb8");
	drain();

	zassert_equal(read_records(), 1);
	zassert_str_equal(records[0].text,
			  "a\tb\nc" FFFD "d" FFFD "[0m " FFFD " \xd0\x9f\xd1\x80\xd0\xb8");
}

ZTEST(zephyr_log_source, test_a_long_message_is_cut_at_512_on_a_character_boundary)
{
	static char big[601];
	static char wide[601];

	memset(big, 'a', 600);
	/* 511 ASCII bytes, then a two-byte letter that does not fit. */
	memset(wide, 'b', 511);
	memcpy(&wide[511], "\xc3\xa9", 2);
	memset(&wide[513], 'c', 87);

	LOG_INF("%s", big);
	LOG_INF("%s", wide);
	drain();

	zassert_equal(read_records(), 2);
	zassert_equal(records[0].text_len, LOG_STORE_TEXT_MAX);
	zassert_true(records[0].truncated);
	zassert_equal(records[1].text_len, 511);
	zassert_true(records[1].truncated);

	struct zephyr_log_source_stats stats;

	zephyr_log_source_get_stats(&stats);
	zassert_true(stats.truncated >= 2);
}

ZTEST(zephyr_log_source, test_output_past_the_raw_buffer_is_cut)
{
	static uint8_t blob[512];

	for (size_t i = 0; i < sizeof(blob); i++) {
		blob[i] = (uint8_t)i;
	}
	/* About 3.5 KB formatted: far past CONFIG_ZEPHYR_LOG_SOURCE_RAW_MAX. */
	LOG_HEXDUMP_WRN(blob, sizeof(blob), "big");
	drain();

	zassert_equal(read_records(), 1);
	zassert_true(records[0].truncated);
	zassert_true(records[0].text_len <= LOG_STORE_TEXT_MAX);
}

ZTEST(zephyr_log_source, test_messages_the_core_dropped_are_counted)
{
	const int flood = 300;
	uint64_t dropped_before = log_store_dropped(STM32_ONLY);
	uint64_t appended_before = appended();
	struct zephyr_log_source_stats before_stats;
	struct zephyr_log_source_stats after_stats;

	zephyr_log_source_get_stats(&before_stats);
	for (int i = 0; i < flood; i++) {
		LOG_INF("flood message number %d with some padding text", i);
	}
	k_msleep(2);
	drain();

	uint64_t dropped = log_store_dropped(STM32_ONLY) - dropped_before;
	uint64_t stored = appended() - appended_before;

	zephyr_log_source_get_stats(&after_stats);
	zassert_true(dropped > 0, "a 4 KiB buffer cannot hold %d messages", flood);
	zassert_equal(dropped + stored, (uint64_t)flood, "dropped %llu + stored %llu", dropped,
		      stored);
	zassert_equal(after_stats.dropped_reported - before_stats.dropped_reported, dropped);
	zassert_equal(after_stats.stored - before_stats.stored, stored);
	zassert_equal(log_store_dropped(LOG_STORE_SOURCE_BIT(LOG_STORE_ESP32)), 0,
		      "an STM32 loss is not an ESP32 one");
}

ZTEST(zephyr_log_source, test_the_backend_never_logs)
{
	struct zephyr_log_source_stats before_stats;
	struct zephyr_log_source_stats after_stats;

	zephyr_log_source_get_stats(&before_stats);
	for (int i = 0; i < 10; i++) {
		LOG_INF("once %d", i);
	}
	zassert_equal(log_buffered_cnt(), 10);
	drain();

	/* Processing produced no new message: nothing waits, and every record is
	 * one of the ten. */
	zassert_equal(log_buffered_cnt(), 0);
	zassert_false(log_process());
	zephyr_log_source_get_stats(&after_stats);
	zassert_equal(after_stats.processed - before_stats.processed, 10);
	zassert_equal(read_records(), 10);
	for (size_t i = 0; i < record_count; i++) {
		zassert_true(strncmp(records[i].text, "once ", 5) == 0, "%s", records[i].text);
	}
}

ZTEST(zephyr_log_source, test_sanitise_rules)
{
	static const struct {
		const char *name;
		const char *in;
		size_t len;
		const char *out;
	} cases[] = {
		{"plain", "abc", 3, "abc"},
		{"crlf", "a\r\nb", 4, "a\nb"},
		{"lone cr", "a\rb", 3, "ab"},
		{"trailing lf", "a\n\n\n", 4, "a"},
		{"tab", "a\tb", 3, "a\tb"},
		{"controls collapse", "a\x01\x02\x7f" "b", 5, "a" FFFD "b"},
		{"overlong", "\xc0\xaf", 2, FFFD},
		/* Found by mutation: E0 needs A0..BF next, or it is an overlong '/'. */
		{"overlong three-byte", "\xe0\x80\xaf" "a", 4, FFFD "a"},
		{"surrogate", "\xed\xa0\x80", 3, FFFD},
		{"above U+10FFFF", "\xf4\x90\x80\x80", 4, FFFD},
		{"truncated sequence", "x\xe2\x82", 3, "x" FFFD},
		{"valid four-byte", "\xf0\x9f\x98\x80", 4, "\xf0\x9f\x98\x80"},
		{"separate runs", "x\xffy\xffz", 5, "x" FFFD "y" FFFD "z"},
	};
	char out[64];

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		bool cut = false;
		size_t n = zephyr_log_source_sanitise((const uint8_t *)cases[i].in, cases[i].len, out,
						      sizeof(out), &cut);

		zassert_equal(n, strlen(cases[i].out), "%s", cases[i].name);
		zassert_mem_equal(out, cases[i].out, n, "%s", cases[i].name);
		zassert_false(cut, "%s", cases[i].name);
	}

	/* A cut never leaves half a character, and says so. */
	bool cut = false;
	size_t n = zephyr_log_source_sanitise((const uint8_t *)"ab\xc3\xa9", 4, out, 3, &cut);

	zassert_equal(n, 2);
	zassert_true(cut);

	/* An input already cut stays marked. */
	cut = true;
	n = zephyr_log_source_sanitise((const uint8_t *)"ok", 2, out, sizeof(out), &cut);
	zassert_equal(n, 2);
	zassert_true(cut);
}

void log_from_the_first_source(void);

/* Found by mutation: source id 0 is a module like any other (src/first.c). */
ZTEST(zephyr_log_source, test_the_module_with_source_id_0_is_named)
{
	zassert_str_equal(log_source_name_get(Z_LOG_LOCAL_DOMAIN_ID, 0), "aaa_first",
			  "precondition: aaa_first sorts first");
	log_from_the_first_source();
	drain();

	zassert_equal(read_records(), 1);
	zassert_true(records[0].has_module);
	zassert_str_equal(records[0].module, "aaa_first");
	zassert_str_equal(records[0].text, "from the first source");
}

/* Found by mutation: output cut at the raw buffer stays marked even when what is
 * left collapses to far less than 512 bytes (1500 control bytes are one U+FFFD). */
ZTEST(zephyr_log_source, test_a_cut_raw_output_is_truncated_even_if_it_shrinks)
{
	static char controls[1501];

	memset(controls, 0x01, 1500);
	LOG_INF("%s", controls);
	drain();

	zassert_equal(read_records(), 1);
	zassert_str_equal(records[0].text, FFFD);
	zassert_true(records[0].truncated, "the raw buffer cut the message");
}

/* Found by mutation: a message the store refuses (here the store alone was
 * told to panic, which log_store_init() in before() undoes) is a counted loss. */
ZTEST(zephyr_log_source, test_a_message_the_store_refuses_is_counted)
{
	uint64_t dropped_before = log_store_dropped(STM32_ONLY);
	struct zephyr_log_source_stats before_stats;
	struct zephyr_log_source_stats after_stats;

	zephyr_log_source_get_stats(&before_stats);
	log_store_panic();
	LOG_INF("refused");
	drain();

	zephyr_log_source_get_stats(&after_stats);
	zassert_equal(after_stats.store_refused - before_stats.store_refused, 1);
	zassert_equal(after_stats.stored - before_stats.stored, 0);
	zassert_equal(log_store_dropped(STM32_ONLY) - dropped_before, 1);
}

/* Last by name: a panic cannot be undone in this process. */
ZTEST(zephyr_log_source, test_zz_after_a_panic_messages_are_only_counted)
{
	uint64_t dropped_before = log_store_dropped(STM32_ONLY);
	uint64_t appended_before = appended();
	struct zephyr_log_source_stats before_stats;
	struct zephyr_log_source_stats after_stats;

	zephyr_log_source_get_stats(&before_stats);
	log_panic();
	/* In panic mode the core processes a message at once, in this context. */
	LOG_ERR("after the panic");
	LOG_INF("and another");

	zephyr_log_source_get_stats(&after_stats);
	zassert_equal(after_stats.after_panic - before_stats.after_panic, 2);
	zassert_equal(appended() - appended_before, 0, "nothing reaches the ring");
	zassert_equal(log_store_dropped(STM32_ONLY) - dropped_before, 2);

	/* The store refuses directly too, without its lock. */
	const struct log_store_entry entry = {
		.source = LOG_STORE_STM32,
		.level = LOG_STORE_LEVEL_INFO,
		.text = "x",
		.text_len = 1,
	};

	zassert_equal(log_store_append(&entry), -EAGAIN);
}
