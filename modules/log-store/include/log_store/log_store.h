/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * log-store: the bounded record rings behind the log screen and the export.
 *
 * Contract: "Логи" in docs/device-development/api-contract.md, LogRecord and
 * LogPage in openapi.json, section 7 of the development plan. The shape, and
 * why:
 *
 * - **One byte ring per source, records of variable length.** STM32 and ESP32
 *   each get their own ring (CONFIG_LOG_STORE_STM32_KIB, _ESP32_KIB), so a C6
 *   printing a ROM error forever cannot push the STM32's history out, and the
 *   other way round. A record is a fixed header, the module name and the text,
 *   and a trailer carrying its length so the ring can be walked backwards (the
 *   contract's "newest `limit` records" without a cursor). Records are aligned
 *   to four bytes.
 *
 * - **Positions are absolute byte offsets** - bytes ever written to that ring -
 *   not offsets into the buffer. A position is still readable exactly when it
 *   is not behind the ring's tail, so a reader that fell behind a wrap is told
 *   so by arithmetic, and a cursor that points past the head is recognisably
 *   not one this device issued.
 *
 * - **One sequence for both sources, assigned under the store's lock.** The
 *   contract orders `source=all` by `seq`, assigned "at capture". The lock
 *   makes commit order equal sequence order, so a reader merging the two rings
 *   never sees seq 10 before seq 9 has landed.
 *
 * - **The producer never waits for a reader longer than one record copy.**
 *   Readers take the same lock, but only to peek at the next header of each
 *   ring and copy one record (at most LOG_STORE_RECORD_MAX bytes) into their
 *   own buffer; everything else - filtering, JSON, sending - happens outside
 *   it. A browser that stalls mid-response holds nothing. The lock is a mutex,
 *   not a spinlock: a spinlock would mask interrupts for the copy, and the
 *   USART3 receive interrupt must not wait on a web client. Nothing here is
 *   called from an ISR.
 *
 * - **A read is bounded twice**: by matching records (the caller's limit) and
 *   by records looked at (the scan budget). A filter that matches nothing
 *   still returns within the budget, with a position that continues the scan -
 *   the contract's "cursor points at the next scan position even when nothing
 *   matched" - and no filter makes a client rescan what it already passed.
 *
 * - **Losses are counted where they happen, never logged.** The Zephyr log
 *   backend feeds this store; a log call from here would be a message about a
 *   message. Records that never reached a ring (the log core dropped them, the
 *   UART overflowed) are counted per source by the producer; records the ring
 *   pushed out are counted by the store. The API's `dropped_count` is the
 *   first kind; the second is what `gap` tells a reader about.
 *
 * Threads: append from any thread (the log thread, the coprocessor worker);
 * read from any thread. Not from an ISR. After log_store_panic() appends are
 * refused without taking the lock, so a panicking log backend can call in from
 * a fault handler safely.
 */

#ifndef LOG_STORE_H_
#define LOG_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest message text in UTF-8 bytes (plan section 9, LogRecord). */
#define LOG_STORE_TEXT_MAX 512

/** Longest module name in bytes (LogRecord.module maxLength). */
#define LOG_STORE_MODULE_MAX 64

/**
 * Longest `contains` filter in UTF-8 bytes. The query parameter's maxLength is
 * 128 code points of up to four bytes each, so a 70-character Cyrillic search
 * must fit: 512 bytes.
 */
#define LOG_STORE_CONTAINS_MAX 512

/** Longest encoded cursor, NUL included. The contract allows 512. */
#define LOG_STORE_CURSOR_MAX 64

enum log_store_source {
	LOG_STORE_STM32 = 0,
	LOG_STORE_ESP32,

	LOG_STORE_SOURCE_COUNT
};

#define LOG_STORE_SOURCE_BIT(s) (1U << (s))
#define LOG_STORE_SOURCES_ALL   (LOG_STORE_SOURCE_BIT(LOG_STORE_STM32) | LOG_STORE_SOURCE_BIT(LOG_STORE_ESP32))

/**
 * A record's level. NONE is JSON null: a line that has no level at all (a
 * printk, a marker). UNKNOWN is a line that might have had one and could not be
 * parsed (UART output). Both survive every min_level filter.
 */
enum log_store_level {
	LOG_STORE_LEVEL_NONE = 0,
	LOG_STORE_LEVEL_DEBUG,
	LOG_STORE_LEVEL_INFO,
	LOG_STORE_LEVEL_WARNING,
	LOG_STORE_LEVEL_ERROR,
	LOG_STORE_LEVEL_UNKNOWN,
};

enum log_store_kind {
	LOG_STORE_KIND_MESSAGE = 0,
	/** The C6 was reset or the UART came back: a new source_generation starts. */
	LOG_STORE_KIND_RESET,
	/** The UART was handed to someone else; nothing arrives until a reset marker. */
	LOG_STORE_KIND_PAUSED,
	/** Only in an export: records between the neighbours were overwritten. */
	LOG_STORE_KIND_GAP,
};

/** Why a record never reached its ring. */
enum log_store_loss {
	/** The Zephyr log core dropped it, or it arrived while the store was unusable. */
	LOG_STORE_LOSS_BACKEND = 0,
	/** Received bytes were lost before a line could be assembled. */
	LOG_STORE_LOSS_UART,

	LOG_STORE_LOSS_COUNT
};

/** What a producer hands in. The store copies it; nothing is retained. */
struct log_store_entry {
	enum log_store_source source;
	enum log_store_level level;
	enum log_store_kind kind;
	/** The producer already cut the text. The store sets it too if it cuts. */
	bool truncated;
	uint32_t generation;
	uint64_t uptime_ms;
	/** NULL for a JSON null module. Longer than LOG_STORE_MODULE_MAX is cut. */
	const char *module;
	size_t module_len;
	/**
	 * UTF-8. The store does not validate it - producers sanitise - but it cuts
	 * at LOG_STORE_TEXT_MAX on a code point boundary and marks the record
	 * truncated, so a record can never hold half a character.
	 */
	const char *text;
	size_t text_len;
};

/** A record as a reader gets it: a copy, valid after the lock is gone. */
struct log_store_record {
	enum log_store_source source;
	enum log_store_level level;
	enum log_store_kind kind;
	bool truncated;
	uint32_t generation;
	uint64_t seq;
	uint64_t uptime_ms;
	bool has_module;
	char module[LOG_STORE_MODULE_MAX + 1];
	uint16_t text_len;
	/** NUL-terminated; text_len bytes of UTF-8 before the NUL. */
	char text[LOG_STORE_TEXT_MAX + 1];
};

/** Selects records. A zeroed filter with sources set matches everything. */
struct log_store_filter {
	/**
	 * LOG_STORE_SOURCE_BIT() of each source to include. 0 matches nothing: a
	 * reader with it returns END at once and stands at both heads, so its
	 * cursor continues from now (used for a filter no record can match, such
	 * as a module name longer than LOG_STORE_MODULE_MAX).
	 */
	uint8_t sources;
	/** NONE for no floor. NONE and UNKNOWN records always pass. */
	enum log_store_level min_level;
	bool has_module;
	/** Exact match. */
	char module[LOG_STORE_MODULE_MAX + 1];
	bool has_contains;
	/** Substring of the text, ASCII letters compared case-insensitively. */
	char contains[LOG_STORE_CONTAINS_MAX + 1];
};

/**
 * Where a read continues. A plain value: copy it before a read to be able to
 * put a record back (the page writer does, when the record does not fit).
 */
struct log_store_reader {
	struct log_store_filter filter;
	/** Next position to look at in each ring. */
	uint64_t pos[LOG_STORE_SOURCE_COUNT];
	/** Stop before this sequence number; 0 for no bound (see log_store_next_seq()). */
	uint64_t upper_seq;
	/**
	 * A position fell behind its ring's tail and was moved to it: records
	 * between were overwritten. Set by the store, cleared by the caller.
	 */
	bool gap;
	/**
	 * Records looked at, matching or not. Reset to 0 by
	 * log_store_reader_tail() and log_store_reader_at(), accumulated by them
	 * and by every log_store_read(); a caller spending one scan budget across
	 * several reads (a page) compares against it and may reset it.
	 */
	uint32_t scanned;
};

enum log_store_read_result {
	/** @p out holds the next matching record. */
	LOG_STORE_READ_RECORD = 0,
	/** Caught up: nothing more to look at now (or the upper bound was reached). */
	LOG_STORE_READ_END,
	/** The scan budget ran out; the reader's position continues the scan. */
	LOG_STORE_READ_BUDGET,
};

/** Per-source figures, for GET /logs/sources and the shell. */
struct log_store_stats {
	uint64_t appended;
	uint64_t overwritten;
	uint64_t lost[LOG_STORE_LOSS_COUNT];
	/** Bytes occupied by records now, and the ring's size. */
	uint32_t used_bytes;
	uint32_t capacity_bytes;
	uint32_t records;
};

/** How long the lock was held, measured on the target (for reports/p5). */
struct log_store_timing {
	uint32_t append_hold_max_us;
	uint32_t append_wait_max_us;
	uint32_t read_hold_max_us;
};

/**
 * @brief Empty both rings and start the sequence at 1.
 *
 * Before the first append; idempotent in that calling it again empties the
 * store again (the sim tier relies on that).
 */
int log_store_init(void);

/** @brief Whether log_store_init() has run. */
bool log_store_ready(void);

/**
 * @brief Add a record.
 *
 * @retval 0        stored, possibly after overwriting the oldest records
 * @retval -EINVAL  bad source, level or kind
 * @retval -EAGAIN  not initialised, or after log_store_panic(); the caller
 *                  counts it as LOG_STORE_LOSS_BACKEND
 */
int log_store_append(const struct log_store_entry *entry);

/** @brief Refuse appends from now on, without ever taking the lock again. */
void log_store_panic(void);

/** @brief Count @p n records of @p source that never reached the ring. */
void log_store_count_loss(enum log_store_source source, enum log_store_loss loss, uint32_t n);

/** @brief What `dropped_count` reports: records lost before the ring, for @p sources. */
uint64_t log_store_dropped(uint8_t sources);

void log_store_get_stats(enum log_store_source source, struct log_store_stats *out);
void log_store_get_timing(struct log_store_timing *out);

/** @brief The sequence number the next append will get. */
uint64_t log_store_next_seq(void);

/**
 * @brief Position @p reader so that it returns the newest @p count matching
 *        records, oldest first, and then whatever arrives after them.
 *
 * Walks both rings backwards from their heads, looking at no more than
 * @p scan_budget records. Fewer matches than @p count is not an error.
 */
void log_store_reader_tail(struct log_store_reader *reader, const struct log_store_filter *filter,
			   uint32_t count, uint32_t scan_budget);

/**
 * @brief Position @p reader at @p pos (from a decoded cursor).
 *
 * A position behind its ring's tail is moved to the tail and sets gap.
 */
void log_store_reader_at(struct log_store_reader *reader, const struct log_store_filter *filter,
			 const uint64_t pos[LOG_STORE_SOURCE_COUNT]);

/**
 * @brief The next matching record, looking at no more than @p scan_budget
 *        records (matching or not) in this call.
 */
enum log_store_read_result log_store_read(struct log_store_reader *reader,
					  struct log_store_record *out, uint32_t scan_budget);

/**
 * @brief Whether @p filter would pass @p record. Exposed for the tests and for
 *        callers that already hold a copy.
 */
bool log_store_filter_match(const struct log_store_filter *filter,
			    const struct log_store_record *record);

/** @brief A 32-bit tag of the boot identifier a cursor is bound to. */
uint32_t log_store_boot_tag(const char *boot_id);

/**
 * @brief Encode where @p reader stands as an opaque cursor.
 *
 * The cursor carries a version, @p boot_tag, a digest of the filter, both
 * positions and a checksum, as unpadded base64url.
 *
 * @return the length written, or -ENOSPC.
 */
int log_store_cursor_encode(const struct log_store_reader *reader, uint32_t boot_tag, char *out,
			    size_t cap);

/**
 * @brief Decode a cursor for @p filter.
 *
 * @retval 0        @p pos is where to continue
 * @retval -ESTALE  a well-formed cursor of another boot: the contract answers
 *                  with the current tail, the new boot_id and gap=true
 * @retval -EINVAL  not a cursor this device issued, one for different filters,
 *                  or a position past the head: 400 invalid_cursor
 */
int log_store_cursor_decode(const char *cursor, uint32_t boot_tag,
			    const struct log_store_filter *filter,
			    uint64_t pos[LOG_STORE_SOURCE_COUNT]);

/** @brief Wire names: "stm32"/"esp32", "debug".."unknown" (NULL for NONE), "message".."gap". */
const char *log_store_source_str(enum log_store_source source);
const char *log_store_level_str(enum log_store_level level);
const char *log_store_kind_str(enum log_store_kind kind);

/**
 * @brief Length of the longest prefix of @p text (at most @p cap bytes) that
 *        does not end inside a UTF-8 sequence.
 *
 * Shared by the producers so that every cut in the system is made the same way.
 */
size_t log_store_utf8_cut(const char *text, size_t len, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LOG_STORE_H_ */
