/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp32-log-source: bytes from the C6's UART turned into log lines.
 *
 * Contract: section 7 of the development plan ("UART line max 512 bytes,
 * partial lines flushed by timeout, ANSI strip, invalid UTF-8 replaced") and
 * the Logs row of section 11 (burst, long line, invalid UTF-8/ANSI, binary UART
 * ROM data). The shape, and why:
 *
 * - **This is the pure part.** The assembler is fed bytes and a clock and hands
 *   out finished lines through a callback. It never touches a UART, a thread or
 *   the store: the board's coprocessor service owns the interrupt (which only
 *   copies bytes into a ring buffer) and runs this on its worker, and connects
 *   the callback to log-store with the C6's current generation. So every rule
 *   below is tested in sim, including on the real ROM stream captured from
 *   board B (tests/fixtures/esp32).
 *
 * - **Everything that reaches a line is valid UTF-8 without control
 *   characters.** CR, LF and CRLF end a line; an empty line is no line. ANSI
 *   escape sequences (CSI `ESC [ ... final`, and two-byte `ESC x`) are removed.
 *   Tab is kept. Every other control byte, every byte that is not part of a
 *   well-formed UTF-8 sequence (overlong forms, surrogates, above U+10FFFF, a
 *   sequence cut by the line's end) becomes U+FFFD, and a run of replacements
 *   is one U+FFFD: a burst of NULs from a C6 in reset should cost three bytes,
 *   not fill the line.
 *
 * - **A line is at most LOG_STORE_TEXT_MAX bytes.** Past that the line is
 *   emitted with truncated=true, cut on a code point boundary, and the rest of
 *   it up to the next line end is discarded. One line of input is one record.
 *
 * - **A partial line is flushed after CONFIG_ESP32_LOG_SOURCE_IDLE_FLUSH_MS
 *   without a byte**, so a prompt or a line the C6 never finished still shows
 *   up; whatever arrives later starts a new line.
 *
 * - **ESP-IDF lines are parsed**: `L (time) tag: text`, where L is E, W, I, D
 *   or V and time is digits (or `hh:mm:ss.mmm`). Level and module come from the
 *   prefix, the text is what follows `tag: `, so an ESP32 record reads like an
 *   STM32 one. V maps to debug. Anything else is level unknown, module null,
 *   and the whole line is the text. The C6's own time is dropped: the record's
 *   time is when the STM32 received the line (plan section 7).
 *
 * - **A ROM banner is flagged.** A line beginning `ESP-ROM:` is what the C6
 *   prints when it starts, whoever reset it; coprocessor-manager uses the flag
 *   to notice a reset it did not cause.
 */

#ifndef ESP32_LOG_SOURCE_H_
#define ESP32_LOG_SOURCE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <log_store/log_store.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One finished line. Pointers are valid only during the callback. */
struct esp32_log_line {
	/** LOG_STORE_LEVEL_UNKNOWN when the line had no ESP-IDF prefix. */
	enum log_store_level level;
	/** The ESP-IDF tag, or NULL. */
	const char *module;
	size_t module_len;
	const char *text;
	size_t text_len;
	bool truncated;
	/** The line starts with "ESP-ROM:": the chip has just started. */
	bool rom_banner;
};

typedef void (*esp32_log_emit_t)(void *ctx, const struct esp32_log_line *line);

struct esp32_log_stats {
	uint32_t lines;
	uint32_t truncated_lines;
	/** Invalid or control bytes replaced (a run counts once). */
	uint32_t replacements;
	uint32_t escape_sequences;
	uint32_t idle_flushes;
	/** esp32_log_note_overflow() calls. */
	uint32_t overflows;
};

/** Private state; declared here so the caller can own it statically. */
struct esp32_log_assembler {
	esp32_log_emit_t emit;
	void *ctx;
	uint32_t idle_flush_ms;

	char line[LOG_STORE_TEXT_MAX + 4];
	size_t len;
	bool truncated;
	bool discarding;
	bool last_was_replacement;
	bool after_cr;
	/* UTF-8 decoder: bytes of the sequence being collected. */
	uint8_t seq[4];
	uint8_t seq_len;
	uint8_t seq_need;
	/* ANSI: 0 none, 1 after ESC, 2 inside CSI. */
	uint8_t escape;
	/* Parameter and intermediate bytes seen in the CSI so far. */
	uint8_t csi_len;
	/* Bytes of "ESP-ROM:" matched at the end of the input so far. */
	uint8_t banner_match;
	bool has_partial;
	int64_t last_byte_ms;

	struct esp32_log_stats stats;
};

/**
 * @brief Start an assembler. @p emit is called from inside feed/poll/flush on
 *        the caller's thread.
 */
void esp32_log_init(struct esp32_log_assembler *a, esp32_log_emit_t emit, void *ctx,
		    uint32_t idle_flush_ms);

/** @brief Consume received bytes; @p now_ms is the time they were taken from the UART. */
void esp32_log_feed(struct esp32_log_assembler *a, const uint8_t *data, size_t len,
		    int64_t now_ms);

/** @brief Emit a partial line that has seen no byte for idle_flush_ms. */
void esp32_log_poll(struct esp32_log_assembler *a, int64_t now_ms);

/** @brief Emit a partial line now: the UART is about to be handed to someone else. */
void esp32_log_flush(struct esp32_log_assembler *a);

/**
 * @brief Bytes were lost before they reached the assembler (the receive ring
 *        overflowed): the line being built is damaged. It gets a U+FFFD and is
 *        marked truncated.
 */
void esp32_log_note_overflow(struct esp32_log_assembler *a);

/** @brief Forget a partial line without emitting it, and reset the decoders. */
void esp32_log_reset(struct esp32_log_assembler *a);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_LOG_SOURCE_H_ */
