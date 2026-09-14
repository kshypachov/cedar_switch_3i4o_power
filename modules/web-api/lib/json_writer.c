/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded JSON writer. Design in include/web_api/json_writer.h.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <api_validation/api_validation.h>
#include <web_api/json_writer.h>

enum level_kind {
	KIND_ROOT = 0,
	KIND_OBJECT,
	KIND_ARRAY,
};

void web_json_writer_init(struct web_json_writer *w, char *buf, size_t cap)
{
	memset(w, 0, sizeof(*w));
	w->buf = buf;
	w->cap = cap;
	if (buf == NULL || cap == 0U) {
		w->failed = true;
		return;
	}
	buf[0] = '\0';
}

static void put(struct web_json_writer *w, const char *text, size_t len)
{
	if (w->failed) {
		return;
	}
	if (w->pos + len >= w->cap) {
		w->failed = true;
		return;
	}
	memcpy(&w->buf[w->pos], text, len);
	w->pos += len;
	w->buf[w->pos] = '\0';
}

static void put_str(struct web_json_writer *w, const char *text)
{
	put(w, text, strlen(text));
}

/*
 * Every value goes through here first: it enforces where a value may appear
 * and writes the comma that separates it from the previous one.
 */
static void begin_value(struct web_json_writer *w)
{
	if (w->failed) {
		return;
	}
	switch (w->kind[w->depth]) {
	case KIND_ROOT:
		if (w->has_value[0]) {
			w->failed = true; /* a document holds one value */
		}
		break;
	case KIND_OBJECT:
		if (!w->after_key) {
			w->failed = true; /* a member needs its name first */
		}
		w->after_key = false;
		break;
	case KIND_ARRAY:
		if (w->has_value[w->depth]) {
			put(w, ",", 1);
		}
		break;
	default:
		w->failed = true;
		break;
	}
}

static void end_value(struct web_json_writer *w)
{
	if (!w->failed) {
		w->has_value[w->depth] = true;
	}
}

static void open_level(struct web_json_writer *w, enum level_kind kind, const char *token)
{
	begin_value(w);
	if (w->failed) {
		return;
	}
	if (w->depth >= WEB_JSON_WRITER_MAX_DEPTH) {
		w->failed = true;
		return;
	}
	put(w, token, 1);
	w->depth++;
	w->kind[w->depth] = (uint8_t)kind;
	w->has_value[w->depth] = false;
}

static void close_level(struct web_json_writer *w, enum level_kind kind, const char *token)
{
	if (w->failed) {
		return;
	}
	if (w->depth == 0U || w->kind[w->depth] != kind || w->after_key) {
		w->failed = true;
		return;
	}
	put(w, token, 1);
	w->depth--;
	end_value(w);
}

void web_json_object_begin(struct web_json_writer *w)
{
	open_level(w, KIND_OBJECT, "{");
}

void web_json_object_end(struct web_json_writer *w)
{
	close_level(w, KIND_OBJECT, "}");
}

void web_json_array_begin(struct web_json_writer *w)
{
	open_level(w, KIND_ARRAY, "[");
}

void web_json_array_end(struct web_json_writer *w)
{
	close_level(w, KIND_ARRAY, "]");
}

void web_json_key(struct web_json_writer *w, const char *name)
{
	if (w->failed) {
		return;
	}
	if (w->kind[w->depth] != KIND_OBJECT || w->after_key || name == NULL) {
		w->failed = true;
		return;
	}
	if (w->has_value[w->depth]) {
		put(w, ",", 1);
	}
	put(w, "\"", 1);
	put_str(w, name);
	put(w, "\":", 2);
	w->after_key = !w->failed;
}

void web_json_string(struct web_json_writer *w, const char *text)
{
	int rc;

	if (text == NULL) {
		w->failed = true;
		return;
	}
	begin_value(w);
	put(w, "\"", 1);
	if (w->failed) {
		return;
	}
	rc = api_json_append_escaped(w->buf, w->cap, w->pos, text);
	if (rc < 0) {
		w->failed = true;
		return;
	}
	w->pos = (size_t)rc;
	put(w, "\"", 1);
	end_value(w);
}

void web_json_string_or_null(struct web_json_writer *w, const char *text)
{
	if (text == NULL) {
		web_json_null(w);
	} else {
		web_json_string(w, text);
	}
}

void web_json_bool(struct web_json_writer *w, bool value)
{
	begin_value(w);
	put_str(w, value ? "true" : "false");
	end_value(w);
}

void web_json_null(struct web_json_writer *w)
{
	begin_value(w);
	put_str(w, "null");
	end_value(w);
}

void web_json_int(struct web_json_writer *w, int64_t value)
{
	char digits[24];

	begin_value(w);
	(void)snprintf(digits, sizeof(digits), "%" PRId64, value);
	put_str(w, digits);
	end_value(w);
}

void web_json_decimal(struct web_json_writer *w, uint64_t value)
{
	char digits[24];

	begin_value(w);
	(void)snprintf(digits, sizeof(digits), "\"%" PRIu64 "\"", value);
	put_str(w, digits);
	end_value(w);
}

void web_json_writer_rollback(struct web_json_writer *w, const struct web_json_writer *saved)
{
	*w = *saved;
	if (w->buf != NULL && w->pos < w->cap) {
		w->buf[w->pos] = '\0';
	}
}

size_t web_json_writer_room(const struct web_json_writer *w)
{
	if (w->failed || w->pos + 1U >= w->cap) {
		return 0U;
	}

	return w->cap - w->pos - 1U;
}

int web_json_writer_finish(struct web_json_writer *w)
{
	if (w->failed || w->depth != 0U || w->after_key || !w->has_value[0]) {
		return -ENOMEM;
	}

	return (int)w->pos;
}
