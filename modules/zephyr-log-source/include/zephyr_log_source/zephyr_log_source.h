/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * zephyr-log-source: the Zephyr log backend that turns the STM32's own log
 * messages into log-store records.
 *
 * Contract: section 3 of the development plan ("Zephyr log backend не пишет в
 * HTTP и не логирует собственное переполнение через тот же backend") and
 * section 7. The shape, and why:
 *
 * - **A deferred-mode backend** (LOG_BACKEND_DEFINE, autostart). The log core
 *   calls it on its processing thread, after the message was written, so the
 *   code that logged never waits for the store. The record's time is the
 *   message's own timestamp, taken when it was logged.
 *
 * - **The text is the message and nothing else.** log_output_process() formats
 *   it without timestamp, level, colours or source name; level and module go to
 *   their own fields (a printk has neither: level null, module null). A hexdump
 *   keeps its lines, separated by LF. Debug messages carry the function name
 *   the log core puts in their format (CONFIG_LOG_FUNC_NAME_PREFIX_DBG).
 *
 * - **The text is sanitised like the ESP32's**: strict UTF-8, CR removed (so a
 *   CRLF becomes LF), LF and tab kept, every other control byte and every
 *   malformed sequence U+FFFD with a run collapsed to one, cut to 512 bytes on
 *   a code point boundary with truncated=true. ANSI sequences are not parsed:
 *   this output has no colours, and an ESC inside a message is shown as U+FFFD.
 *
 * - **No recursion, ever.** Nothing in this module logs. What goes wrong is a
 *   counter: messages the core dropped (the backend's dropped callback), records
 *   the store refused, messages processed after a panic. All of them are also
 *   LOG_STORE_LOSS_BACKEND in the store, which is what `dropped_count` reports.
 *
 * - **Panic.** After log_panic() the core calls the backend synchronously from
 *   whatever context panicked, possibly a fault handler. The backend then only
 *   counts: it never touches the store's mutex or its formatting buffers again.
 *
 * - **Not ready before the store.** is_ready() answers -EBUSY until
 *   log_store_init() has run; the log thread polls it. With
 *   CONFIG_ZEPHYR_LOG_SOURCE_INIT_STORE the store is initialised at POST_KERNEL,
 *   before any thread, so the log thread activates the backend on its first pass
 *   and every message written during boot still waits in the core's buffer for
 *   it. Only what overflowed that buffer is lost - and counted.
 */

#ifndef ZEPHYR_LOG_SOURCE_H_
#define ZEPHYR_LOG_SOURCE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Counters since boot. Read without a lock; each is individually atomic. */
struct zephyr_log_source_stats {
	/** Messages the backend was given. */
	uint32_t processed;
	/** Of those, stored as records. */
	uint32_t stored;
	/** Messages the log core reported dropped before they reached backends. */
	uint32_t dropped_reported;
	/** The store refused the record (not initialised, or panicked). */
	uint32_t store_refused;
	/** Messages that arrived after the panic and were only counted. */
	uint32_t after_panic;
	/** Records marked truncated. */
	uint32_t truncated;
};

void zephyr_log_source_get_stats(struct zephyr_log_source_stats *out);

/**
 * @brief Make @p in (a formatted message) into record text.
 *
 * The rules above: strict UTF-8, CR dropped, LF and tab kept, other controls
 * and malformed sequences U+FFFD with runs collapsed, trailing LFs removed, at
 * most @p cap bytes cut on a code point boundary.
 *
 * @param cut  in: the input was already cut; out: set when this cut it too.
 * @return the length written to @p out (not NUL-terminated).
 */
size_t zephyr_log_source_sanitise(const uint8_t *in, size_t len, char *out, size_t cap, bool *cut);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_LOG_SOURCE_H_ */
