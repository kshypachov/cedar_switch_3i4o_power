/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The ESP32 line assembler: every rule of esp32_log_source.h, and the ROM
 * stream board B's C6 actually prints.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp32_log_source/esp32_log_source.h>

#define IDLE_MS   CONFIG_ESP32_LOG_SOURCE_IDLE_FLUSH_MS
#define MAX_LINES 1400
#define FFFD      "\xEF\xBF\xBD"

struct got {
	enum log_store_level level;
	bool has_module;
	char module[LOG_STORE_MODULE_MAX + 1];
	char text[LOG_STORE_TEXT_MAX + 1];
	size_t text_len;
	bool truncated;
	bool rom_banner;
};

static struct got lines[MAX_LINES];
static size_t count;
static struct esp32_log_assembler as;
static int64_t now;

static void collect(void *ctx, const struct esp32_log_line *l)
{
	struct got *g;

	ARG_UNUSED(ctx);
	zassert_true(count < MAX_LINES, "more lines than the suite keeps");
	zassert_true(l->text_len <= LOG_STORE_TEXT_MAX, "a line of %zu bytes", l->text_len);
	zassert_true(l->module_len <= LOG_STORE_MODULE_MAX);

	g = &lines[count++];
	memset(g, 0, sizeof(*g));
	g->level = l->level;
	g->has_module = l->module != NULL;
	if (g->has_module) {
		memcpy(g->module, l->module, l->module_len);
	}
	memcpy(g->text, l->text, l->text_len);
	g->text_len = l->text_len;
	g->truncated = l->truncated;
	g->rom_banner = l->rom_banner;
}

static void start(void)
{
	esp32_log_init(&as, collect, NULL, IDLE_MS);
	count = 0;
	now = 1000;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	start();
}

static void feedn(const uint8_t *data, size_t len)
{
	esp32_log_feed(&as, data, len, now);
}

static void feed(const char *s)
{
	feedn((const uint8_t *)s, strlen(s));
}

static void assert_line(size_t i, const char *text)
{
	zassert_true(i < count, "line %zu of %zu", i, count);
	zassert_equal(lines[i].text_len, strlen(text), "line %zu: \"%s\" (%zu) != \"%s\"", i,
		      lines[i].text, lines[i].text_len, text);
	zassert_mem_equal(lines[i].text, text, lines[i].text_len, "line %zu: \"%s\"", i,
			  lines[i].text);
}

/* Strict UTF-8 with no control characters but tab: what every line must be. */
static bool clean_text(const char *s, size_t n)
{
	size_t i = 0;

	while (i < n) {
		uint8_t b = (uint8_t)s[i];
		uint32_t cp;
		size_t need;

		if (b < 0x80) {
			if ((b < 0x20 && b != '\t') || b == 0x7F) {
				return false;
			}
			i++;
			continue;
		}
		if (b >= 0xC2 && b <= 0xDF) {
			need = 1;
			cp = b & 0x1F;
		} else if (b >= 0xE0 && b <= 0xEF) {
			need = 2;
			cp = b & 0x0F;
		} else if (b >= 0xF0 && b <= 0xF4) {
			need = 3;
			cp = b & 0x07;
		} else {
			return false;
		}
		if (i + need >= n) {
			return false;
		}
		for (size_t k = 1; k <= need; k++) {
			uint8_t c = (uint8_t)s[i + k];

			if ((c & 0xC0) != 0x80) {
				return false;
			}
			cp = (cp << 6) | (c & 0x3F);
		}
		if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
		    (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
			return false;
		}
		i += need + 1;
	}
	return true;
}

static void assert_all_clean(void)
{
	for (size_t i = 0; i < count; i++) {
		zassert_true(clean_text(lines[i].text, lines[i].text_len), "line %zu: \"%s\"", i,
			     lines[i].text);
		zassert_true(lines[i].text_len > 0 || lines[i].has_module, "an empty record %zu",
			     i);
		if (lines[i].has_module) {
			zassert_not_equal(lines[i].level, LOG_STORE_LEVEL_UNKNOWN);
			zassert_true(strlen(lines[i].module) > 0);
			zassert_is_null(strchr(lines[i].module, ':'));
			zassert_true(clean_text(lines[i].module, strlen(lines[i].module)));
		} else {
			zassert_equal(lines[i].level, LOG_STORE_LEVEL_UNKNOWN);
		}
	}
}

ZTEST_SUITE(esp32_log_source, NULL, NULL, before, NULL, NULL);

/* -- line ends ------------------------------------------------------------ */

ZTEST(esp32_log_source, test_cr_lf_and_crlf_end_a_line_and_empty_lines_are_skipped)
{
	feed("a\rb\r\nc\n\nd\r\r\ne\n\r\n");

	zassert_equal(count, 5);
	assert_line(0, "a");
	assert_line(1, "b");
	assert_line(2, "c");
	assert_line(3, "d");
	assert_line(4, "e");
	for (size_t i = 0; i < count; i++) {
		zassert_equal(lines[i].level, LOG_STORE_LEVEL_UNKNOWN);
		zassert_false(lines[i].has_module);
		zassert_false(lines[i].truncated);
		zassert_false(lines[i].rom_banner);
	}
	zassert_equal(as.stats.lines, 5);
}

ZTEST(esp32_log_source, test_nothing_is_emitted_before_a_line_ends)
{
	feed("no end yet");
	zassert_equal(count, 0);
	esp32_log_poll(&as, now + IDLE_MS - 1);
	zassert_equal(count, 0, "not idle long enough");
}

/* -- fragments ------------------------------------------------------------ */

/* Every rule at once, so that a split anywhere crosses one of them. */
static const uint8_t mixed[] =
	"\x1b[0;33mW (1234) wifi: AP not found\x1b[0m\r\n"
	"plain \xc3\xa9t\xc3\xa9 \xff\xfe line\n"
	"ESP-ROM:esp32c6-20220919\r\n"
	"I (00:00:01.234) boot: time\n"
	"x\xe2\x82\n"
	"\x00\x01tab\there\r"
	"\xf0\x9f\x98\x80 emoji\n"
	"invalid header: 0xff\xe6" "ESP-ROM:esp32c6-20220919\r\n";

#define MIXED_LEN (sizeof(mixed) - 1)
#define REF_MAX   16

static struct got ref[REF_MAX];
static size_t ref_count;

static void assert_same_as_ref(const char *how)
{
	zassert_equal(count, ref_count, "%s: %zu lines, expected %zu", how, count, ref_count);
	for (size_t i = 0; i < count; i++) {
		zassert_mem_equal(&lines[i], &ref[i], sizeof(ref[i]), "%s: line %zu \"%s\"", how,
				  i, lines[i].text);
	}
}

ZTEST(esp32_log_source, test_the_mixed_input_gives_the_expected_lines)
{
	feedn(mixed, MIXED_LEN);

	zassert_equal(count, 9);
	assert_line(0, "AP not found");
	zassert_equal(lines[0].level, LOG_STORE_LEVEL_WARNING);
	zassert_str_equal(lines[0].module, "wifi");
	assert_line(1, "plain \xc3\xa9t\xc3\xa9 " FFFD " line");
	assert_line(2, "ESP-ROM:esp32c6-20220919");
	zassert_true(lines[2].rom_banner);
	assert_line(3, "time");
	zassert_equal(lines[3].level, LOG_STORE_LEVEL_INFO);
	zassert_str_equal(lines[3].module, "boot");
	assert_line(4, "x" FFFD);
	assert_line(5, FFFD "tab\there");
	assert_line(6, "\xf0\x9f\x98\x80 emoji");
	assert_line(7, "invalid header: 0xff" FFFD);
	zassert_false(lines[7].rom_banner);
	assert_line(8, "ESP-ROM:esp32c6-20220919");
	zassert_true(lines[8].rom_banner);
	assert_all_clean();
}

ZTEST(esp32_log_source, test_a_split_at_any_byte_gives_the_same_lines)
{
	feedn(mixed, MIXED_LEN);
	zassert_true(count <= REF_MAX);
	memcpy(ref, lines, count * sizeof(lines[0]));
	ref_count = count;

	for (size_t cut = 0; cut <= MIXED_LEN; cut++) {
		char how[24];

		start();
		feedn(mixed, cut);
		feedn(&mixed[cut], MIXED_LEN - cut);
		snprintf(how, sizeof(how), "cut at %zu", cut);
		assert_same_as_ref(how);
	}

	start();
	for (size_t i = 0; i < MIXED_LEN; i++) {
		feedn(&mixed[i], 1);
	}
	assert_same_as_ref("byte by byte");
}

/* -- idle flush and handover ---------------------------------------------- */

ZTEST(esp32_log_source, test_a_partial_line_is_flushed_after_the_idle_time)
{
	feed("prompt> ");
	esp32_log_poll(&as, now + IDLE_MS - 1);
	zassert_equal(count, 0);
	esp32_log_poll(&as, now + IDLE_MS);
	zassert_equal(count, 1);
	assert_line(0, "prompt> ");
	zassert_false(lines[0].truncated, "nothing was cut; the line was just not finished");
	zassert_equal(as.stats.idle_flushes, 1);

	/* What comes later is a line of its own. */
	now += IDLE_MS + 50;
	feed("rest\n");
	zassert_equal(count, 2);
	assert_line(1, "rest");

	/* Nothing partial: polling emits nothing and counts nothing. */
	esp32_log_poll(&as, now + 10 * IDLE_MS);
	zassert_equal(count, 2);
	zassert_equal(as.stats.idle_flushes, 1);
}

ZTEST(esp32_log_source, test_a_byte_restarts_the_idle_time)
{
	feed("slow");
	now += IDLE_MS - 10;
	feed(" printer");
	esp32_log_poll(&as, now + IDLE_MS - 1);
	zassert_equal(count, 0, "the last byte came %d ms ago", IDLE_MS - 1);
	esp32_log_poll(&as, now + IDLE_MS);
	assert_line(0, "slow printer");
}

ZTEST(esp32_log_source, test_a_cut_sequence_is_replaced_when_the_line_goes_idle)
{
	feed("half \xe2\x82");
	esp32_log_poll(&as, now + IDLE_MS);
	zassert_equal(count, 1);
	assert_line(0, "half " FFFD);
}

ZTEST(esp32_log_source, test_idle_after_a_truncated_line_ends_the_discarding)
{
	char big[LOG_STORE_TEXT_MAX + 40];

	memset(big, 'z', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	feed(big);
	zassert_equal(count, 1);
	zassert_true(lines[0].truncated);
	esp32_log_poll(&as, now + IDLE_MS);
	zassert_equal(count, 1, "the dropped rest is not a line");
	now += IDLE_MS;
	feed("fresh\n");
	zassert_equal(count, 2);
	assert_line(1, "fresh");
}

ZTEST(esp32_log_source, test_flush_emits_the_partial_line_at_a_handover)
{
	esp32_log_flush(&as);
	zassert_equal(count, 0, "nothing to flush");

	feed("before the flasher took the UART");
	esp32_log_flush(&as);
	zassert_equal(count, 1);
	assert_line(0, "before the flasher took the UART");

	/* A CR just before the handover does not swallow the next owner's LF. */
	feed("x\r");
	esp32_log_flush(&as);
	feed("\nafter\n");
	zassert_equal(count, 3);
	assert_line(1, "x");
	assert_line(2, "after");
}

ZTEST(esp32_log_source, test_reset_forgets_a_partial_line)
{
	feed("\x1b[3");
	feed("garbage \xe2");
	esp32_log_reset(&as);
	feed("m clean\n");
	zassert_equal(count, 1);
	assert_line(0, "m clean");
}

/* -- ANSI ------------------------------------------------------------------ */

ZTEST(esp32_log_source, test_colour_codes_are_removed_and_the_level_still_parses)
{
	feed("\x1b[0;32mI (412) boot: compile time 11:22:33\x1b[0m\r\n");
	zassert_equal(count, 1);
	assert_line(0, "compile time 11:22:33");
	zassert_equal(lines[0].level, LOG_STORE_LEVEL_INFO);
	zassert_str_equal(lines[0].module, "boot");
	zassert_equal(as.stats.escape_sequences, 2);
}

ZTEST(esp32_log_source, test_two_byte_and_broken_escapes)
{
	/* ESC c (reset terminal) is two bytes; ESC followed by a control byte
	 * loses only the ESC; a CSI cut by a line end is abandoned there. */
	feed("\x1b" "cafter reset\n");
	feed("a\x1b\x01z\n");
	feed("cut \x1b[12\nnext\n");
	feed("\x1b[1;31;4;5mbold\x1b[K\n");

	zassert_equal(count, 5);
	assert_line(0, "after reset");
	assert_line(1, "a" FFFD "z");
	assert_line(2, "cut ");
	assert_line(3, "next");
	assert_line(4, "bold");
}

ZTEST(esp32_log_source, test_an_overlong_csi_is_abandoned)
{
	char s[64] = "\x1b[";

	memset(s + 2, '1', 40);
	strcat(s, "Xtext\n");
	feed(s);
	zassert_equal(count, 1);
	/* The first 32 parameter bytes belong to the escape; the rest is text. */
	assert_line(0, "11111111Xtext");
}

/* -- ESP-IDF prefixes ------------------------------------------------------ */

ZTEST(esp32_log_source, test_esp_idf_prefixes)
{
	static const struct {
		const char *in;
		enum log_store_level level;
		const char *module; /* NULL: not parsed */
		const char *text;
	} cases[] = {
		{"E (1) wifi: failed", LOG_STORE_LEVEL_ERROR, "wifi", "failed"},
		{"W (22) wifi: weak", LOG_STORE_LEVEL_WARNING, "wifi", "weak"},
		{"I (333) boot: ESP-IDF v5.5.5", LOG_STORE_LEVEL_INFO, "boot", "ESP-IDF v5.5.5"},
		{"D (4444) phy: cal", LOG_STORE_LEVEL_DEBUG, "phy", "cal"},
		{"V (5) phy: very", LOG_STORE_LEVEL_DEBUG, "phy", "very"},
		{"I (00:00:01.234) main_task: Started", LOG_STORE_LEVEL_INFO, "main_task", "Started"},
		{"I (12) esp image: seg 0", LOG_STORE_LEVEL_INFO, "esp image", "seg 0"},
		{"I (1) tag:", LOG_STORE_LEVEL_INFO, "tag", ""},
		{"I (1) tag: a: b: c", LOG_STORE_LEVEL_INFO, "tag", "a: b: c"},
		{"X (1) tag: x", LOG_STORE_LEVEL_UNKNOWN, NULL, "X (1) tag: x"},
		{"i (1) tag: x", LOG_STORE_LEVEL_UNKNOWN, NULL, "i (1) tag: x"},
		{"I (12)boot: x", LOG_STORE_LEVEL_UNKNOWN, NULL, "I (12)boot: x"},
		{"I () boot: x", LOG_STORE_LEVEL_UNKNOWN, NULL, "I () boot: x"},
		{"I (a1) boot: x", LOG_STORE_LEVEL_UNKNOWN, NULL, "I (a1) boot: x"},
		{"I (1) : x", LOG_STORE_LEVEL_UNKNOWN, NULL, "I (1) : x"},
		{"I (1) tag:x", LOG_STORE_LEVEL_UNKNOWN, NULL, "I (1) tag:x"},
		{"I (1) no colon", LOG_STORE_LEVEL_UNKNOWN, NULL, "I (1) no colon"},
		{"I (1", LOG_STORE_LEVEL_UNKNOWN, NULL, "I (1"},
		{"rst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)", LOG_STORE_LEVEL_UNKNOWN, NULL,
		 "rst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		start();
		feed(cases[i].in);
		feed("\n");
		zassert_equal(count, 1, "%s", cases[i].in);
		zassert_equal(lines[0].level, cases[i].level, "%s", cases[i].in);
		zassert_equal(lines[0].has_module, cases[i].module != NULL, "%s", cases[i].in);
		if (cases[i].module != NULL) {
			zassert_str_equal(lines[0].module, cases[i].module, "%s", cases[i].in);
		}
		assert_line(0, cases[i].text);
	}
}

ZTEST(esp32_log_source, test_a_tag_of_64_bytes_parses_and_65_does_not)
{
	char in[128];

	memset(in, 0, sizeof(in));
	strcpy(in, "W (7) ");
	memset(in + 6, 't', 64);
	strcat(in, ": ok\n");
	feed(in);
	zassert_equal(lines[0].level, LOG_STORE_LEVEL_WARNING);
	zassert_equal(strlen(lines[0].module), 64);
	assert_line(0, "ok");

	start();
	memset(in, 0, sizeof(in));
	strcpy(in, "W (7) ");
	memset(in + 6, 't', 65);
	strcat(in, ": ok\n");
	feed(in);
	zassert_equal(lines[0].level, LOG_STORE_LEVEL_UNKNOWN);
	zassert_false(lines[0].has_module);
}

/* -- UTF-8 and control bytes ---------------------------------------------- */

ZTEST(esp32_log_source, test_invalid_utf8_and_control_bytes)
{
	static const struct {
		const char *name;
		const char *in;
		size_t len;
		const char *out;
	} cases[] = {
		{"two-byte", "\xc3\xa9", 2, "\xc3\xa9"},
		{"four-byte", "\xf0\x9f\x98\x80", 4, "\xf0\x9f\x98\x80"},
		{"U+10FFFF", "\xf4\x8f\xbf\xbf", 4, "\xf4\x8f\xbf\xbf"},
		{"overlong C0", "\xc0\xaf", 2, FFFD},
		{"overlong E0", "\xe0\x80\x80", 3, FFFD},
		{"overlong F0", "\xf0\x80\x80\x80", 4, FFFD},
		{"surrogate", "\xed\xa0\x80", 3, FFFD},
		{"above U+10FFFF", "\xf4\x90\x80\x80", 4, FFFD},
		{"F5", "\xf5\x80", 2, FFFD},
		{"FF", "\xff", 1, FFFD},
		{"stray continuation", "a\x80\x80\x80z", 5, "a" FFFD "z"},
		{"cut by an ASCII byte", "\xe2\x82" "a", 3, FFFD "a"},
		{"cut by a new lead", "\xe2\xc3\xa9", 3, FFFD "\xc3\xa9"},
		{"cut by the line end", "a\xe2\x82", 3, "a" FFFD},
		{"separate runs", "x\xffy\xffz", 5, "x" FFFD "y" FFFD "z"},
		{"NULs", "\x00\x00\x00\x00ok", 6, FFFD "ok"},
		{"controls", "\x01\x02\x7f\x08ok", 6, FFFD "ok"},
		{"tab kept", "a\tb", 3, "a\tb"},
		{"only garbage", "\xff\xfe\x00", 3, FFFD},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		start();
		feedn((const uint8_t *)cases[i].in, cases[i].len);
		feed("\n");
		zassert_equal(count, 1, "%s", cases[i].name);
		zassert_equal(lines[0].text_len, strlen(cases[i].out), "%s: \"%s\"", cases[i].name,
			      lines[0].text);
		zassert_mem_equal(lines[0].text, cases[i].out, lines[0].text_len, "%s",
				  cases[i].name);
	}
}

static uint32_t rng = 0x2545F491;

static uint32_t next_random(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

ZTEST(esp32_log_source, test_random_bytes_never_produce_an_unclean_line)
{
	/* Bytes that start or end structure, so garbage keeps hitting the rules
	 * rather than being plain text. */
	static const uint8_t interesting[] = {
		0x00, '\n', '\r', 0x1B, '[', 'm', 'E', 'I', 'W', ' ', '(', ')', ':', '0',
		0x80, 0xBF, 0xC2, 0xE0, 0xED, 0xF0, 0xF4, 0xFF, '\t', 0x7F, 'S', 'P', '-',
		'R', 'O', 'M',
	};
	uint8_t buf[2048];
	uint32_t total_lines = 0;

	for (int round = 0; round < 300; round++) {
		start();
		for (size_t i = 0; i < sizeof(buf); i++) {
			uint32_t r = next_random();

			buf[i] = (r & 1U) ? interesting[(r >> 8) % sizeof(interesting)]
					  : (uint8_t)(r >> 16);
		}
		for (size_t i = 0; i < sizeof(buf);) {
			size_t n = MIN(1U + next_random() % 64U, sizeof(buf) - i);

			feedn(&buf[i], n);
			i += n;
			if ((next_random() & 7U) == 0U) {
				now += next_random() % (2U * IDLE_MS);
				esp32_log_poll(&as, now);
			}
			if ((next_random() & 63U) == 0U) {
				esp32_log_note_overflow(&as);
			}
		}
		esp32_log_flush(&as);
		assert_all_clean();
		zassert_equal(as.stats.lines, count);
		total_lines += count;
	}
	zassert_true(total_lines > 1000, "the garbage made %u lines", total_lines);
}

/* -- long lines ------------------------------------------------------------ */

ZTEST(esp32_log_source, test_a_long_line_is_cut_at_512_and_the_rest_dropped)
{
	char big[700];

	memset(big, 'a', 600);
	big[600] = '\0';
	feed(big);
	zassert_equal(count, 1, "emitted as soon as it overflows");
	feed("\nnext\n");

	zassert_equal(count, 2);
	zassert_equal(lines[0].text_len, LOG_STORE_TEXT_MAX);
	zassert_true(lines[0].truncated);
	assert_line(1, "next");
	zassert_false(lines[1].truncated);
	zassert_equal(as.stats.truncated_lines, 1);
}

ZTEST(esp32_log_source, test_exactly_512_bytes_is_not_truncated)
{
	char big[LOG_STORE_TEXT_MAX + 2];

	memset(big, 'b', LOG_STORE_TEXT_MAX);
	big[LOG_STORE_TEXT_MAX] = '\n';
	big[LOG_STORE_TEXT_MAX + 1] = '\0';
	feed(big);
	zassert_equal(count, 1);
	zassert_equal(lines[0].text_len, LOG_STORE_TEXT_MAX);
	zassert_false(lines[0].truncated);
}

ZTEST(esp32_log_source, test_the_cut_never_splits_a_character)
{
	char big[LOG_STORE_TEXT_MAX + 8];

	memset(big, 'c', LOG_STORE_TEXT_MAX - 1);
	big[LOG_STORE_TEXT_MAX - 1] = '\0';
	strcat(big, "\xc3\xa9tail\n");
	feed(big);
	zassert_equal(count, 1);
	zassert_equal(lines[0].text_len, LOG_STORE_TEXT_MAX - 1);
	zassert_true(lines[0].truncated);
	zassert_true(clean_text(lines[0].text, lines[0].text_len));
}

ZTEST(esp32_log_source, test_a_replacement_run_that_overflows_is_cut_cleanly)
{
	char big[LOG_STORE_TEXT_MAX + 8];

	memset(big, 'd', LOG_STORE_TEXT_MAX - 2);
	big[LOG_STORE_TEXT_MAX - 2] = '\0';
	feed(big);
	feedn((const uint8_t *)"\xff\xff\xff", 3);
	feed("\n");
	zassert_equal(count, 1);
	zassert_equal(lines[0].text_len, LOG_STORE_TEXT_MAX - 2);
	zassert_true(lines[0].truncated);
}

/* -- overflow -------------------------------------------------------------- */

ZTEST(esp32_log_source, test_lost_bytes_mark_the_line_truncated)
{
	feed("abc\xe2");
	esp32_log_note_overflow(&as);
	feed("def\n");
	zassert_equal(count, 1);
	assert_line(0, "abc" FFFD "def");
	zassert_true(lines[0].truncated);
	zassert_equal(as.stats.overflows, 1);

	/* The next line is whole again. */
	feed("ok\n");
	zassert_false(lines[1].truncated);
}

ZTEST(esp32_log_source, test_an_overflow_between_lines_is_still_a_damaged_line)
{
	esp32_log_note_overflow(&as);
	feed("tail of something\n");
	zassert_equal(count, 1);
	assert_line(0, FFFD "tail of something");
	zassert_true(lines[0].truncated);
}

ZTEST(esp32_log_source, test_an_overflow_while_discarding_adds_no_line)
{
	char big[600];

	memset(big, 'e', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	feed(big);
	esp32_log_note_overflow(&as);
	feed("still the long line\nnext\n");
	zassert_equal(count, 2);
	zassert_true(lines[0].truncated);
	assert_line(1, "next");
	zassert_false(lines[1].truncated);
}

/* -- ROM banner ------------------------------------------------------------ */

ZTEST(esp32_log_source, test_a_banner_starts_its_own_line_wherever_it_lands)
{
	feed("ESP-ROM:esp32c6-20220919\r\n");
	feed("invalid header: 0xff");
	feedn((const uint8_t *)"\xe6", 1);
	feed("ESP-ROM:esp32c6-20220919\r\n");
	feed("ESP-RO ESP-ESP-ROM:x\n");

	zassert_equal(count, 5);
	assert_line(0, "ESP-ROM:esp32c6-20220919");
	zassert_true(lines[0].rom_banner);
	assert_line(1, "invalid header: 0xff" FFFD);
	zassert_false(lines[1].truncated, "cut by a reset, not by the cap");
	assert_line(2, "ESP-ROM:esp32c6-20220919");
	zassert_true(lines[2].rom_banner);
	assert_line(3, "ESP-RO ESP-");
	assert_line(4, "ESP-ROM:x");
	zassert_true(lines[4].rom_banner);
}

ZTEST(esp32_log_source, test_a_banner_inside_the_dropped_rest_of_a_long_line)
{
	char big[600];

	memset(big, 'f', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	feed(big);
	feed("ESP-ROM:esp32c6-20220919\r\n");

	zassert_equal(count, 2);
	zassert_true(lines[0].truncated);
	assert_line(1, "ESP-ROM:esp32c6-20220919");
	zassert_true(lines[1].rom_banner);
}

ZTEST(esp32_log_source, test_a_banner_split_by_garbage_or_a_line_end_is_not_a_banner)
{
	feed("ESP-\xffROM:x\n");
	feed("ESP-ROM\n:x\n");
	feed("ESP-ROM");
	esp32_log_poll(&as, now + IDLE_MS);
	feed(":y\n");
	esp32_log_note_overflow(&as);
	feed("ESP-ROM");
	esp32_log_note_overflow(&as);
	feed(":z\n");
	zassert_equal(count, 6);
	assert_line(0, "ESP-" FFFD "ROM:x");
	assert_line(1, "ESP-ROM");
	assert_line(2, ":x");
	assert_line(3, "ESP-ROM");
	assert_line(4, ":y");
	assert_line(5, FFFD "ESP-ROM" FFFD ":z");
	for (size_t i = 0; i < count; i++) {
		zassert_false(lines[i].rom_banner, "line %zu", i);
	}
}

/* -- the ROM stream captured on board B ----------------------------------- */

static const uint8_t fixture[] = {
#include "esp32_rom_fixture.inc"
};

/*
 * tests/fixtures/esp32/board-b-rom-link-check.log is the console of
 * tests/coprocessor_link_check on board B (2026-09-13): the STM32's own boot
 * log, then everything USART3 received from the C6 between two marker lines
 * the test printed. The test printed non-printable bytes as <hh>, and the
 * console turned CRLF into LF. Only the part between the markers is the C6.
 */
static const char begin_marker[] = "--- normal boot (expect ROM banner + invalid header) ---\n";
static const char end_marker[] = "--- normal boot (expect ROM banner + invalid header): ";

static size_t find(const uint8_t *hay, size_t n, const char *needle)
{
	size_t k = strlen(needle);

	for (size_t i = 0; i + k <= n; i++) {
		if (memcmp(&hay[i], needle, k) == 0) {
			return i;
		}
	}
	return SIZE_MAX;
}

static void region(const uint8_t **start, size_t *len)
{
	size_t b = find(fixture, sizeof(fixture), begin_marker);
	size_t e = find(fixture, sizeof(fixture), end_marker);

	zassert_not_equal(b, SIZE_MAX);
	zassert_not_equal(e, SIZE_MAX);
	b += strlen(begin_marker);
	zassert_true(e > b);
	*start = &fixture[b];
	*len = e - b;
}

static size_t lines_equal_to(const char *text)
{
	size_t n = 0;

	for (size_t i = 0; i < count; i++) {
		n += (lines[i].text_len == strlen(text) &&
		      memcmp(lines[i].text, text, lines[i].text_len) == 0)
			     ? 1U
			     : 0U;
	}
	return n;
}

static size_t banners(void)
{
	size_t n = 0;

	for (size_t i = 0; i < count; i++) {
		if (lines[i].rom_banner) {
			zassert_equal(lines[i].text_len, strlen("ESP-ROM:esp32c6-20220919"));
			n++;
		}
	}
	return n;
}

static void feed_in_chunks(const uint8_t *data, size_t len, size_t chunk)
{
	for (size_t i = 0; i < len; i += chunk) {
		feedn(&data[i], MIN(chunk, len - i));
		now += 1;
		esp32_log_poll(&as, now);
	}
	esp32_log_flush(&as);
}

ZTEST(esp32_log_source, test_the_rom_stream_as_captured)
{
	const uint8_t *c6;
	size_t len;

	region(&c6, &len);
	zassert_equal(len, 27524, "the fixture changed");
	feed_in_chunks(c6, len, 37);

	/* 1016 lines by line ends, plus the three the resets cut in two. */
	zassert_equal(count, 1019);
	zassert_equal(lines_equal_to("invalid header: 0xffffffff"), 983);
	zassert_equal(lines_equal_to("rst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)"), 1);
	zassert_equal(lines_equal_to("rst:0x7 (TG0_WDT_HPSYS),boot:0xc (SPI_FAST_FLASH_BOOT)"), 3,
		      "the ROM's watchdog restarts the chip with no firmware");
	zassert_equal(banners(), 3, "the power-on banner was lost by the capture");
	zassert_equal(lines_equal_to("invalid header: 0xff<e6>"), 1);
	zassert_equal(as.stats.truncated_lines, 0);
	assert_all_clean();
}

ZTEST(esp32_log_source, test_the_rom_stream_with_its_bytes_restored)
{
	static uint8_t raw[32768];
	const uint8_t *c6;
	size_t len;
	size_t n = 0;
	size_t restored = 0;

	region(&c6, &len);
	/* Undo the capture's rendering: <hh> back to the byte, LF back to CRLF. */
	for (size_t i = 0; i < len; i++) {
		unsigned int byte;

		if (c6[i] == '<' && i + 3 < len && c6[i + 3] == '>' &&
		    sscanf((const char *)&c6[i + 1], "%2x", &byte) == 1) {
			raw[n++] = (uint8_t)byte;
			i += 3;
			restored++;
		} else if (c6[i] == '\n') {
			raw[n++] = '\r';
			raw[n++] = '\n';
		} else {
			raw[n++] = c6[i];
		}
		zassert_true(n + 2 < sizeof(raw));
	}
	zassert_true(restored >= 3, "restored %zu bytes", restored);

	feed_in_chunks(raw, n, 64);

	zassert_equal(count, 1019);
	assert_line(0, FFFD "-20919");
	zassert_equal(lines_equal_to("invalid header: 0xffffffff"), 983);
	zassert_equal(lines_equal_to("invalid header: 0xff" FFFD), 1);
	zassert_equal(banners(), 3);
	assert_all_clean();
}

/* -- found by mutation (reports/p5/notes-mutations-logs.md) ------------------- */

/* The shortest ESP-IDF line: one-digit time, one-byte tag, no text (8 bytes). */
ZTEST(esp32_log_source, test_the_shortest_esp_idf_line_parses)
{
	feed("I (0) t:\n");
	zassert_equal(count, 1);
	zassert_equal(lines[0].level, LOG_STORE_LEVEL_INFO);
	zassert_true(lines[0].has_module);
	zassert_str_equal(lines[0].module, "t");
	zassert_equal(lines[0].text_len, 0);
}

/* A line that is the banner and nothing else is still a banner. */
ZTEST(esp32_log_source, test_a_bare_banner_line_is_a_banner)
{
	feed("ESP-ROM:\n");
	zassert_equal(count, 1);
	assert_line(0, "ESP-ROM:");
	zassert_true(lines[0].rom_banner);
}

/* An escape abandons a started character: its continuation bytes after the
 * escape are strays, not the rest of it. */
ZTEST(esp32_log_source, test_an_escape_abandons_a_started_character)
{
	feed("a\xe2\x1b[0m\x82\xac" "b\n");
	zassert_equal(count, 1);
	assert_line(0, "a" FFFD "b");
}

/* The last final byte of a CSI, '~' (0x7E, as in ESC [3~), ends it too. */
ZTEST(esp32_log_source, test_a_csi_ending_in_tilde_is_removed_whole)
{
	feed("a\x1b[3~b\x1b[@c\n");
	zassert_equal(count, 1);
	assert_line(0, "abc");
}
