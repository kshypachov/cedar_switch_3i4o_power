/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded JSON response writing for the web API.
 *
 * Every success body the device sends is built here, into a buffer the caller
 * owns. Three decisions:
 *
 * - **Text is escaped by api-validation's escaper**, the one the error body
 *   uses, so an SSID or a log line is spelled the same in a success response
 *   as in a rejection, and the same as the mock's transcription of that
 *   function.
 *
 * - **Failure is sticky.** Running out of room sets a flag and every later
 *   call does nothing; the caller checks once, at web_json_writer_finish().
 *   A handler that writes fifteen members does not need fifteen error checks,
 *   and cannot forget the one that mattered - a truncated body is never sent,
 *   because finish() reports it.
 *
 * - **Numbers that JavaScript would round are strings.** The contract spells
 *   uptime and sequence numbers as decimal strings; web_json_decimal() is how,
 *   so no handler formats one with a format string of its own.
 *
 * Member names are written verbatim and must be literals from the schema; they
 * are not escaped.
 */

#ifndef WEB_API_JSON_WRITER_H_
#define WEB_API_JSON_WRITER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_JSON_WRITER_MAX_DEPTH 8

struct web_json_writer {
	char *buf;
	size_t cap;
	size_t pos;
	uint8_t depth;
	/** Per level: what is open there (level 0 is the document itself). */
	uint8_t kind[WEB_JSON_WRITER_MAX_DEPTH + 1];
	/** Per level: has a value already been written, so the next needs a comma. */
	bool has_value[WEB_JSON_WRITER_MAX_DEPTH + 1];
	/** A member name was just written and awaits its value. */
	bool after_key;
	bool failed;
};

void web_json_writer_init(struct web_json_writer *w, char *buf, size_t cap);

void web_json_object_begin(struct web_json_writer *w);
void web_json_object_end(struct web_json_writer *w);
void web_json_array_begin(struct web_json_writer *w);
void web_json_array_end(struct web_json_writer *w);

/** A member name; must be followed by exactly one value. */
void web_json_key(struct web_json_writer *w, const char *name);

void web_json_string(struct web_json_writer *w, const char *text);
/** @p text, or null when @p text is NULL. */
void web_json_string_or_null(struct web_json_writer *w, const char *text);
void web_json_bool(struct web_json_writer *w, bool value);
void web_json_null(struct web_json_writer *w);
void web_json_int(struct web_json_writer *w, int64_t value);
/** An unsigned value as a JSON string of decimal digits ("103000"). */
void web_json_decimal(struct web_json_writer *w, uint64_t value);

/**
 * @brief Close the document and report.
 *
 * @return its length in bytes (the buffer is NUL-terminated), or -ENOMEM if it
 *         did not fit or was structurally unfinished (an open object, a key
 *         with no value).
 */
int web_json_writer_finish(struct web_json_writer *w);

#ifdef __cplusplus
}
#endif

#endif /* WEB_API_JSON_WRITER_H_ */
