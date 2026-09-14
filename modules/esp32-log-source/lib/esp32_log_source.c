/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The ESP32 line assembler. Design in
 * include/esp32_log_source/esp32_log_source.h.
 *
 * One byte at a time, with all state in the assembler, so a line split into any
 * fragments comes out exactly as it would have in one piece.
 */

#include <string.h>

#include <esp32_log_source/esp32_log_source.h>

/* A CSI sequence longer than this is not one a terminal would recognise either;
 * the escape is abandoned and the bytes after it are text again. */
#define CSI_MAX_BYTES 32

enum {
	ESC_NONE = 0,
	ESC_AFTER_ESC,
	ESC_CSI,
};

static const char replacement[3] = {(char)0xEF, (char)0xBF, (char)0xBD};

/* -- the line ------------------------------------------------------------- */

static bool is_level_letter(char c, enum log_store_level *level)
{
	switch (c) {
	case 'E':
		*level = LOG_STORE_LEVEL_ERROR;
		return true;
	case 'W':
		*level = LOG_STORE_LEVEL_WARNING;
		return true;
	case 'I':
		*level = LOG_STORE_LEVEL_INFO;
		return true;
	case 'D':
	case 'V':
		*level = LOG_STORE_LEVEL_DEBUG;
		return true;
	default:
		return false;
	}
}

/*
 * `L (time) tag: text`. time is digits, or digits with ':' and '.' (the
 * system-time format, hh:mm:ss.mmm). The tag is 1..64 bytes up to the first
 * ':', which must be followed by a space or end the line.
 */
static bool parse_esp_idf(const char *s, size_t n, struct esp32_log_line *out)
{
	enum log_store_level level;
	size_t i;
	size_t tag_start;

	if (n < 7 || !is_level_letter(s[0], &level) || s[1] != ' ' || s[2] != '(' ||
	    s[3] < '0' || s[3] > '9') {
		return false;
	}
	for (i = 4; i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == ':' || s[i] == '.'); i++) {
	}
	if (i + 1 >= n || s[i] != ')' || s[i + 1] != ' ') {
		return false;
	}
	tag_start = i + 2;
	for (i = tag_start; i < n && s[i] != ':'; i++) {
	}
	if (i >= n || i == tag_start || i - tag_start > LOG_STORE_MODULE_MAX) {
		return false;
	}
	if (i + 1 < n && s[i + 1] != ' ') {
		return false;
	}

	out->level = level;
	out->module = &s[tag_start];
	out->module_len = i - tag_start;
	if (i + 1 < n) {
		out->text = &s[i + 2];
		out->text_len = n - (i + 2);
	} else {
		out->text = &s[n];
		out->text_len = 0;
	}
	return true;
}

static void clear_line(struct esp32_log_assembler *a)
{
	a->len = 0;
	a->truncated = false;
	a->last_was_replacement = false;
	a->seq_len = 0;
	a->seq_need = 0;
}

static void emit(struct esp32_log_assembler *a)
{
	struct esp32_log_line line = {
		.level = LOG_STORE_LEVEL_UNKNOWN,
		.module = NULL,
		.module_len = 0,
		.text = a->line,
		.text_len = a->len,
		.truncated = a->truncated,
	};

	line.rom_banner = a->len >= 8 && memcmp(a->line, "ESP-ROM:", 8) == 0;
	(void)parse_esp_idf(a->line, a->len, &line);

	a->stats.lines++;
	if (line.truncated) {
		a->stats.truncated_lines++;
	}
	if (a->emit != NULL) {
		a->emit(a->ctx, &line);
	}
}

/* Add complete, valid bytes of one code point (or U+FFFD) to the line. */
static void append(struct esp32_log_assembler *a, const char *bytes, size_t n)
{
	if (a->discarding) {
		return;
	}
	if (a->len + n > LOG_STORE_TEXT_MAX) {
		/* One line is one record: the part that fits goes out marked, the
		 * rest of the line is dropped up to its end. */
		a->truncated = true;
		emit(a);
		clear_line(a);
		a->discarding = true;
		return;
	}
	memcpy(&a->line[a->len], bytes, n);
	a->len += n;
}

static void append_char(struct esp32_log_assembler *a, const char *bytes, size_t n)
{
	append(a, bytes, n);
	a->last_was_replacement = false;
}

static void append_replacement(struct esp32_log_assembler *a)
{
	if (a->last_was_replacement) {
		return;
	}
	a->stats.replacements++;
	append(a, replacement, sizeof(replacement));
	/* Set after append(): a truncation inside it cleared the line, and the
	 * run continues into what is discarded either way. */
	a->last_was_replacement = true;
}

/* A UTF-8 sequence was started and will not be completed. */
static void abandon_sequence(struct esp32_log_assembler *a)
{
	if (a->seq_len > 0) {
		a->seq_len = 0;
		a->seq_need = 0;
		append_replacement(a);
	}
}

static void end_line(struct esp32_log_assembler *a)
{
	abandon_sequence(a);
	a->escape = ESC_NONE;
	a->banner_match = 0;
	if (!a->discarding && a->len > 0) {
		emit(a);
	}
	clear_line(a);
	a->discarding = false;
	a->has_partial = false;
}

/* -- the ROM banner -------------------------------------------------------- */

static const char banner[] = "ESP-ROM:";
#define BANNER_LEN (sizeof(banner) - 1)

/*
 * Track "ESP-ROM:" over the text bytes, discarded ones included: a reset can
 * land in the part of a long line that is being dropped. The pattern has no
 * repeated prefix, so a mismatch restarts at 0, or at 1 on an 'E'.
 */
static bool banner_completed(struct esp32_log_assembler *a, char c)
{
	if (c == banner[a->banner_match]) {
		a->banner_match++;
	} else {
		a->banner_match = (c == banner[0]) ? 1U : 0U;
	}
	if (a->banner_match < BANNER_LEN) {
		return false;
	}
	a->banner_match = 0;
	return true;
}

/*
 * The ':' of "ESP-ROM:" arrived. A C6 that resets prints its banner wherever
 * its UART was, so the banner starts a line of its own: what came before it is
 * the line the reset cut (board B: "invalid header: 0xff\xe6ESP-ROM:...").
 */
static void start_banner_line(struct esp32_log_assembler *a)
{
	const size_t head = BANNER_LEN - 1;
	bool tail_is_banner = !a->discarding && a->len >= head &&
			      memcmp(&a->line[a->len - head], banner, head) == 0;

	if (tail_is_banner && a->len == head) {
		/* The banner is already where a line starts. */
		append_char(a, ":", 1);
		return;
	}
	if (tail_is_banner) {
		a->len -= head;
	}
	if (!a->discarding && a->len > 0) {
		emit(a);
	}
	clear_line(a);
	a->discarding = false;
	append_char(a, banner, BANNER_LEN);
}

/* -- UTF-8 ----------------------------------------------------------------- */

/* The allowed range of the second byte for a lead byte (Unicode table 3-7). */
static bool second_byte_ok(uint8_t lead, uint8_t b)
{
	switch (lead) {
	case 0xE0:
		return b >= 0xA0 && b <= 0xBF;
	case 0xED:
		return b >= 0x80 && b <= 0x9F;
	case 0xF0:
		return b >= 0x90 && b <= 0xBF;
	case 0xF4:
		return b >= 0x80 && b <= 0x8F;
	default:
		return b >= 0x80 && b <= 0xBF;
	}
}

static void text_byte(struct esp32_log_assembler *a, uint8_t b)
{
	if (a->seq_len > 0) {
		bool ok = (a->seq_len == 1) ? second_byte_ok(a->seq[0], b)
					    : (b >= 0x80 && b <= 0xBF);

		if (ok) {
			a->seq[a->seq_len++] = b;
			if (a->seq_len == a->seq_need) {
				append_char(a, (const char *)a->seq, a->seq_len);
				a->seq_len = 0;
				a->seq_need = 0;
			}
			return;
		}
		/* The maximal subpart so far is one replacement; the byte that
		 * broke it is looked at afresh. */
		abandon_sequence(a);
	}

	if (b == '\t' || (b >= 0x20 && b < 0x7F)) {
		char c = (char)b;

		if (banner_completed(a, c)) {
			start_banner_line(a);
			return;
		}
		append_char(a, &c, 1);
		return;
	}

	a->banner_match = 0;
	if (b < 0x80) {
		/* A control character (or DEL). */
		append_replacement(a);
	} else if (b >= 0xC2 && b <= 0xDF) {
		a->seq[0] = b;
		a->seq_len = 1;
		a->seq_need = 2;
	} else if (b >= 0xE0 && b <= 0xEF) {
		a->seq[0] = b;
		a->seq_len = 1;
		a->seq_need = 3;
	} else if (b >= 0xF0 && b <= 0xF4) {
		a->seq[0] = b;
		a->seq_len = 1;
		a->seq_need = 4;
	} else {
		/* A stray continuation byte, C0/C1 (overlong) or F5..FF. */
		append_replacement(a);
	}
}

/* -- public ---------------------------------------------------------------- */

void esp32_log_init(struct esp32_log_assembler *a, esp32_log_emit_t emit_fn, void *ctx,
		    uint32_t idle_flush_ms)
{
	memset(a, 0, sizeof(*a));
	a->emit = emit_fn;
	a->ctx = ctx;
	a->idle_flush_ms = idle_flush_ms;
}

static void feed_byte(struct esp32_log_assembler *a, uint8_t b)
{
	bool was_cr = a->after_cr;

	a->after_cr = false;

	if (b == '\r') {
		end_line(a);
		a->after_cr = true;
		return;
	}
	if (b == '\n') {
		if (!was_cr) {
			end_line(a);
		}
		return;
	}

	if (a->escape == ESC_AFTER_ESC) {
		if (b == '[') {
			a->escape = ESC_CSI;
			a->csi_len = 0;
			return;
		}
		a->escape = ESC_NONE;
		if (b >= 0x20 && b < 0x7F) {
			/* ESC x: a two-byte sequence, consumed. */
			return;
		}
		/* ESC followed by something no sequence has: the ESC alone is gone,
		 * the byte is text. */
	} else if (a->escape == ESC_CSI) {
		if (b >= 0x40 && b <= 0x7E) {
			a->escape = ESC_NONE;
			return;
		}
		if (b >= 0x20 && b <= 0x3F && ++a->csi_len <= CSI_MAX_BYTES) {
			/* Parameter or intermediate byte. */
			return;
		}
		a->escape = ESC_NONE;
	}

	if (b == 0x1B) {
		abandon_sequence(a);
		a->escape = ESC_AFTER_ESC;
		a->stats.escape_sequences++;
		return;
	}

	text_byte(a, b);
}

void esp32_log_feed(struct esp32_log_assembler *a, const uint8_t *data, size_t len,
		    int64_t now_ms)
{
	if (len == 0) {
		return;
	}
	for (size_t i = 0; i < len; i++) {
		feed_byte(a, data[i]);
	}
	a->last_byte_ms = now_ms;
	a->has_partial = a->len > 0 || a->seq_len > 0 || a->discarding || a->escape != ESC_NONE;
}

void esp32_log_poll(struct esp32_log_assembler *a, int64_t now_ms)
{
	if (!a->has_partial || now_ms - a->last_byte_ms < (int64_t)a->idle_flush_ms) {
		return;
	}
	if (a->len > 0 || a->seq_len > 0) {
		a->stats.idle_flushes++;
	}
	end_line(a);
	a->after_cr = false;
}

void esp32_log_flush(struct esp32_log_assembler *a)
{
	end_line(a);
	a->after_cr = false;
}

void esp32_log_note_overflow(struct esp32_log_assembler *a)
{
	a->stats.overflows++;
	/* The bytes that would have finished an escape or a character are gone. */
	a->escape = ESC_NONE;
	a->seq_len = 0;
	a->seq_need = 0;
	a->after_cr = false;
	a->banner_match = 0;
	if (a->discarding) {
		/* The line already went out marked truncated. */
		return;
	}
	append_replacement(a);
	a->truncated = true;
	a->has_partial = true;
}

void esp32_log_reset(struct esp32_log_assembler *a)
{
	clear_line(a);
	a->discarding = false;
	a->escape = ESC_NONE;
	a->after_cr = false;
	a->banner_match = 0;
	a->has_partial = false;
}
