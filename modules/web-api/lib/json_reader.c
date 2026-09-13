/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Strict JSON request decoding. Design in include/web_api/json_reader.h;
 * comments here cover only how it is carried out.
 *
 * One pass, recursive descent over the body. Schema problems are collected as
 * the parse goes, but a syntax error anywhere wins: the whole document is
 * still read to the end, so a malformed body is always invalid_json no matter
 * how many schema problems came before the point where it broke.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include <web_api/json_reader.h>

/* Schema problems kept before sorting; more than this marks truncation. */
#define MAX_COLLECTED 32
/* Member names longer than this cannot be ours and are compared by hash only. */
#define NAME_MAX 64
/* Members tracked per object for duplicate detection. */
#define MAX_MEMBERS 64

struct collected {
	char path[API_ERROR_PATH_MAX_LEN + 1];
	enum api_field_code code;
};

struct reader {
	const uint8_t *p;
	const uint8_t *end;
	const char *syntax;
	struct collected fields[MAX_COLLECTED];
	size_t field_count;
	bool truncated;
	char path[API_ERROR_PATH_MAX_LEN + 1];
	size_t path_len;
	uint8_t depth;
	const uint8_t *dest_base;
	size_t dest_size;
};

/* -- errors and paths --------------------------------------------------- */

static void syntax_error(struct reader *r, const char *message)
{
	if (r->syntax == NULL) {
		r->syntax = message;
	}
	/* Stop reading: nothing after a syntax error can be trusted. */
	r->p = r->end;
}

static bool failed(const struct reader *r)
{
	return r->syntax != NULL;
}

static void report(struct reader *r, enum api_field_code code)
{
	struct collected *c;

	if (r->field_count >= MAX_COLLECTED) {
		r->truncated = true;
		return;
	}
	c = &r->fields[r->field_count++];
	if (r->path_len == 0U) {
		c->path[0] = '/';
		c->path[1] = '\0';
	} else {
		memcpy(c->path, r->path, r->path_len + 1U);
	}
	c->code = code;
}

/*
 * Append one RFC 6901 segment. Returns the length to restore afterwards. A
 * segment that does not fit leaves the path at its parent, so a report names
 * the nearest enclosing value rather than a truncated name.
 */
static size_t path_push_bytes(struct reader *r, const char *name, size_t len)
{
	size_t saved = r->path_len;
	char seg[API_ERROR_PATH_MAX_LEN + 1];
	size_t n = 0;

	seg[n++] = '/';
	for (size_t i = 0; i < len; i++) {
		const char *esc = NULL;

		if (name[i] == '~') {
			esc = "~0";
		} else if (name[i] == '/') {
			esc = "~1";
		}
		if (esc != NULL) {
			if (n + 2U > API_ERROR_PATH_MAX_LEN) {
				return saved;
			}
			seg[n++] = esc[0];
			seg[n++] = esc[1];
		} else {
			if ((uint8_t)name[i] < 0x20U || n + 1U > API_ERROR_PATH_MAX_LEN) {
				/* api-validation refuses control characters in a path. */
				return saved;
			}
			seg[n++] = name[i];
		}
	}
	if (saved + n > API_ERROR_PATH_MAX_LEN) {
		return saved;
	}
	memcpy(&r->path[saved], seg, n);
	r->path_len = saved + n;
	r->path[r->path_len] = '\0';

	return saved;
}

static size_t path_push_index(struct reader *r, size_t index)
{
	char digits[12];
	size_t n = 0;
	char rev[12];

	do {
		rev[n++] = (char)('0' + (index % 10U));
		index /= 10U;
	} while (index > 0U && n < sizeof(rev));
	for (size_t i = 0; i < n; i++) {
		digits[i] = rev[n - 1U - i];
	}

	return path_push_bytes(r, digits, n);
}

static void path_pop(struct reader *r, size_t saved)
{
	r->path_len = saved;
	r->path[saved] = '\0';
}

/* -- lexical ------------------------------------------------------------ */

static void skip_ws(struct reader *r)
{
	while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
		r->p++;
	}
}

static bool peek(struct reader *r, uint8_t c)
{
	return r->p < r->end && *r->p == c;
}

static int hex_value(uint8_t c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static bool read_hex4(struct reader *r, uint32_t *out)
{
	uint32_t v = 0;

	if (r->end - r->p < 4) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		int h = hex_value(r->p[i]);

		if (h < 0) {
			return false;
		}
		v = (v << 4) | (uint32_t)h;
	}
	r->p += 4;
	*out = v;

	return true;
}

struct text {
	char *dst;      /* NULL when only validating */
	size_t cap;     /* including the NUL */
	size_t bytes;   /* decoded bytes, counted even past cap */
	size_t chars;   /* code points */
	bool overflow;  /* decoded bytes did not fit in cap */
	bool bad;       /* U+0000 or an unpaired surrogate escape */
	uint64_t hash;  /* FNV-1a of the decoded bytes, for duplicate names */
};

static void text_put(struct text *t, const uint8_t *bytes, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		t->hash ^= bytes[i];
		t->hash *= 0x100000001b3ULL;
	}
	if (t->dst != NULL && !t->overflow) {
		if (t->bytes + n + 1U > t->cap) {
			t->overflow = true;
		} else {
			memcpy(&t->dst[t->bytes], bytes, n);
			t->dst[t->bytes + n] = '\0';
		}
	}
	t->bytes += n;
}

static void text_put_cp(struct text *t, uint32_t cp)
{
	uint8_t b[4];
	size_t n;

	if (cp < 0x80U) {
		b[0] = (uint8_t)cp;
		n = 1;
	} else if (cp < 0x800U) {
		b[0] = (uint8_t)(0xC0U | (cp >> 6));
		b[1] = (uint8_t)(0x80U | (cp & 0x3FU));
		n = 2;
	} else if (cp < 0x10000U) {
		b[0] = (uint8_t)(0xE0U | (cp >> 12));
		b[1] = (uint8_t)(0x80U | ((cp >> 6) & 0x3FU));
		b[2] = (uint8_t)(0x80U | (cp & 0x3FU));
		n = 3;
	} else {
		b[0] = (uint8_t)(0xF0U | (cp >> 18));
		b[1] = (uint8_t)(0x80U | ((cp >> 12) & 0x3FU));
		b[2] = (uint8_t)(0x80U | ((cp >> 6) & 0x3FU));
		b[3] = (uint8_t)(0x80U | (cp & 0x3FU));
		n = 4;
	}
	text_put(t, b, n);
}

/* Validate one raw UTF-8 sequence starting at r->p and copy it. */
static bool read_utf8(struct reader *r, struct text *t)
{
	const uint8_t *s = r->p;
	uint8_t c = s[0];
	uint32_t cp;
	size_t extra;

	if ((c & 0xE0U) == 0xC0U) {
		cp = c & 0x1FU;
		extra = 1;
	} else if ((c & 0xF0U) == 0xE0U) {
		cp = c & 0x0FU;
		extra = 2;
	} else if ((c & 0xF8U) == 0xF0U) {
		cp = c & 0x07U;
		extra = 3;
	} else {
		return false;
	}
	if ((size_t)(r->end - s) <= extra) {
		return false;
	}
	for (size_t k = 1; k <= extra; k++) {
		if ((s[k] & 0xC0U) != 0x80U) {
			return false;
		}
		cp = (cp << 6) | (s[k] & 0x3FU);
	}
	if ((extra == 1U && cp < 0x80U) || (extra == 2U && cp < 0x800U) ||
	    (extra == 3U && cp < 0x10000U) || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
		return false;
	}
	text_put(t, s, extra + 1U);
	t->chars++;
	r->p += extra + 1U;

	return true;
}

/* Precondition: *r->p == '"'. */
static void read_string(struct reader *r, struct text *t)
{
	t->bytes = 0;
	t->chars = 0;
	t->overflow = false;
	t->bad = false;
	t->hash = 0xcbf29ce484222325ULL;
	if (t->dst != NULL && t->cap > 0U) {
		t->dst[0] = '\0';
	}
	r->p++;

	while (r->p < r->end) {
		uint8_t c = *r->p;

		if (c == '"') {
			r->p++;
			return;
		}
		if (c < 0x20U) {
			syntax_error(r, "A control character must be escaped inside a string");
			return;
		}
		if (c >= 0x80U) {
			if (!read_utf8(r, t)) {
				syntax_error(r, "The body is not valid UTF-8");
				return;
			}
			continue;
		}
		if (c != '\\') {
			text_put(t, &c, 1);
			t->chars++;
			r->p++;
			continue;
		}

		r->p++;
		if (r->p >= r->end) {
			break;
		}
		c = *r->p++;
		switch (c) {
		case '"':
		case '\\':
		case '/':
			text_put(t, &c, 1);
			break;
		case 'b':
			text_put(t, (const uint8_t *)"\b", 1);
			break;
		case 'f':
			text_put(t, (const uint8_t *)"\f", 1);
			break;
		case 'n':
			text_put(t, (const uint8_t *)"\n", 1);
			break;
		case 'r':
			text_put(t, (const uint8_t *)"\r", 1);
			break;
		case 't':
			text_put(t, (const uint8_t *)"\t", 1);
			break;
		case 'u': {
			uint32_t cp;

			if (!read_hex4(r, &cp)) {
				syntax_error(r, "A \\u escape needs four hexadecimal digits");
				return;
			}
			if (cp >= 0xD800U && cp <= 0xDBFFU && r->end - r->p >= 6 && r->p[0] == '\\' &&
			    r->p[1] == 'u') {
				const uint8_t *save = r->p;
				uint32_t low;

				r->p += 2;
				if (!read_hex4(r, &low)) {
					syntax_error(r, "A \\u escape needs four hexadecimal digits");
					return;
				}
				if (low >= 0xDC00U && low <= 0xDFFFU) {
					cp = 0x10000U + ((cp - 0xD800U) << 10) + (low - 0xDC00U);
				} else {
					/* Not a pair; the second escape is read on its own. */
					r->p = save;
					t->bad = true;
					t->chars++;
					continue;
				}
			} else if (cp >= 0xD800U && cp <= 0xDFFFU) {
				t->bad = true;
				t->chars++;
				continue;
			}
			if (cp == 0U) {
				t->bad = true;
			} else {
				text_put_cp(t, cp);
			}
			break;
		}
		default:
			syntax_error(r, "Unknown escape in a string");
			return;
		}
		t->chars++;
	}

	syntax_error(r, "A string is not terminated");
}

enum number_kind {
	NUMBER_INTEGRAL,     /* fits in int64_t */
	NUMBER_OUT_OF_RANGE, /* integral, but not an int64_t */
	NUMBER_FRACTIONAL,
};

/*
 * JSON's number grammar, strictly, with the value judged as JSON Schema judges
 * an integer: 120, 120.0 and 1.2e2 are the same integral value.
 */
static enum number_kind read_number(struct reader *r, int64_t *out)
{
	uint8_t digits[40];
	size_t nd = 0;      /* significant digits kept */
	int64_t exp10 = 0;  /* value = digits * 10^exp10 */
	bool negative = false;
	bool any_dropped_nonzero = false;
	const uint8_t *start = r->p;

	*out = 0;
	if (peek(r, '-')) {
		negative = true;
		r->p++;
	}
	if (peek(r, '0')) {
		r->p++;
	} else if (r->p < r->end && *r->p >= '1' && *r->p <= '9') {
		while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
			if (nd < sizeof(digits)) {
				digits[nd++] = *r->p - '0';
			} else {
				exp10++;
			}
			r->p++;
		}
	} else {
		syntax_error(r, "Malformed number");
		return NUMBER_FRACTIONAL;
	}
	if (peek(r, '.')) {
		r->p++;
		if (!(r->p < r->end && *r->p >= '0' && *r->p <= '9')) {
			syntax_error(r, "Malformed number");
			return NUMBER_FRACTIONAL;
		}
		while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
			if (nd < sizeof(digits)) {
				digits[nd++] = *r->p - '0';
				exp10--;
			} else if (*r->p != '0') {
				any_dropped_nonzero = true;
			}
			r->p++;
		}
	}
	if (peek(r, 'e') || peek(r, 'E')) {
		bool exp_negative = false;
		int64_t e = 0;

		r->p++;
		if (peek(r, '+') || peek(r, '-')) {
			exp_negative = *r->p == '-';
			r->p++;
		}
		if (!(r->p < r->end && *r->p >= '0' && *r->p <= '9')) {
			syntax_error(r, "Malformed number");
			return NUMBER_FRACTIONAL;
		}
		while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
			if (e < 100000) {
				e = e * 10 + (*r->p - '0');
			}
			r->p++;
		}
		exp10 += exp_negative ? -e : e;
	}
	ARG_UNUSED(start);

	/* Strip leading and trailing zeros of the significand. */
	size_t first = 0;

	while (first < nd && digits[first] == 0U) {
		first++;
	}
	while (nd > first && digits[nd - 1U] == 0U) {
		nd--;
		exp10++;
	}
	if (first == nd) {
		return any_dropped_nonzero ? NUMBER_FRACTIONAL : NUMBER_INTEGRAL;
	}
	if (exp10 < 0 || any_dropped_nonzero) {
		return NUMBER_FRACTIONAL;
	}
	if ((int64_t)(nd - first) + exp10 > 19) {
		return NUMBER_OUT_OF_RANGE;
	}

	/* Accumulate as a negative number, which has the larger range. */
	int64_t v = 0;

	for (size_t i = first; i < nd; i++) {
		if (v < (INT64_MIN + digits[i]) / 10) {
			return NUMBER_OUT_OF_RANGE;
		}
		v = v * 10 - digits[i];
	}
	for (int64_t i = 0; i < exp10; i++) {
		if (v < INT64_MIN / 10) {
			return NUMBER_OUT_OF_RANGE;
		}
		v *= 10;
	}
	if (!negative) {
		if (v == INT64_MIN) {
			return NUMBER_OUT_OF_RANGE;
		}
		v = -v;
	}
	*out = v;

	return NUMBER_INTEGRAL;
}

static bool read_literal(struct reader *r, const char *word)
{
	size_t n = strlen(word);

	if ((size_t)(r->end - r->p) < n || memcmp(r->p, word, n) != 0) {
		syntax_error(r, "Unexpected token");
		return false;
	}
	r->p += n;

	return true;
}

/* -- generic values ----------------------------------------------------- */

static void skip_value(struct reader *r);

/*
 * An object nobody described: every member is still read and checked for
 * syntax and duplicates. With @p report_unknown each member is reported as
 * `unknown_field` - that is how undeclared members of a described object are
 * handled too, through read_object.
 */
struct members {
	uint64_t hash[MAX_MEMBERS];
	size_t len[MAX_MEMBERS];
	size_t count;
};

static bool member_is_duplicate(struct reader *r, struct members *m, const struct text *name)
{
	for (size_t i = 0; i < m->count; i++) {
		if (m->hash[i] == name->hash && m->len[i] == name->bytes) {
			syntax_error(r, "A member name appears twice in one object");
			return true;
		}
	}
	if (m->count >= MAX_MEMBERS) {
		syntax_error(r, "An object has too many members");
		return true;
	}
	m->hash[m->count] = name->hash;
	m->len[m->count] = name->bytes;
	m->count++;

	return false;
}

static void skip_object(struct reader *r)
{
	struct members members = {0};
	char name_buf[NAME_MAX + 1];

	r->p++; /* { */
	skip_ws(r);
	if (peek(r, '}')) {
		r->p++;
		return;
	}
	for (;;) {
		struct text name = {.dst = name_buf, .cap = sizeof(name_buf)};

		skip_ws(r);
		if (!peek(r, '"')) {
			syntax_error(r, "Expected a member name");
			return;
		}
		read_string(r, &name);
		if (failed(r) || member_is_duplicate(r, &members, &name)) {
			return;
		}
		skip_ws(r);
		if (!peek(r, ':')) {
			syntax_error(r, "Expected ':' after a member name");
			return;
		}
		r->p++;
		skip_value(r);
		if (failed(r)) {
			return;
		}
		skip_ws(r);
		if (peek(r, ',')) {
			r->p++;
			continue;
		}
		if (peek(r, '}')) {
			r->p++;
			return;
		}
		syntax_error(r, "Expected ',' or '}' in an object");
		return;
	}
}

static void skip_array(struct reader *r)
{
	r->p++; /* [ */
	skip_ws(r);
	if (peek(r, ']')) {
		r->p++;
		return;
	}
	for (;;) {
		skip_value(r);
		if (failed(r)) {
			return;
		}
		skip_ws(r);
		if (peek(r, ',')) {
			r->p++;
			continue;
		}
		if (peek(r, ']')) {
			r->p++;
			return;
		}
		syntax_error(r, "Expected ',' or ']' in an array");
		return;
	}
}

static void skip_value(struct reader *r)
{
	skip_ws(r);
	if (r->p >= r->end) {
		syntax_error(r, "Unexpected end of the body");
		return;
	}
	switch (*r->p) {
	case '{':
	case '[':
		if (r->depth >= WEB_JSON_MAX_DEPTH) {
			syntax_error(r, "The body is nested too deeply");
			return;
		}
		r->depth++;
		if (*r->p == '{') {
			skip_object(r);
		} else {
			skip_array(r);
		}
		r->depth--;
		return;
	case '"': {
		struct text t = {0};

		read_string(r, &t);
		return;
	}
	case 't':
		(void)read_literal(r, "true");
		return;
	case 'f':
		(void)read_literal(r, "false");
		return;
	case 'n':
		(void)read_literal(r, "null");
		return;
	default: {
		int64_t ignored;

		if (*r->p == '-' || (*r->p >= '0' && *r->p <= '9')) {
			(void)read_number(r, &ignored);
		} else {
			syntax_error(r, "Unexpected token");
		}
		return;
	}
	}
}

/* -- schema ------------------------------------------------------------- */

static void *slot(struct reader *r, const void *base, size_t offset, size_t size)
{
	const uint8_t *p = (const uint8_t *)base + offset;

	__ASSERT(p >= r->dest_base && p + size <= r->dest_base + r->dest_size,
		 "descriptor offset outside the destination");
	if (p < r->dest_base || p + size > r->dest_base + r->dest_size) {
		return NULL;
	}

	return (void *)p;
}

static void set_flag(struct reader *r, void *base, size_t offset)
{
	bool *flag = slot(r, base, offset, sizeof(bool));

	if (flag != NULL) {
		*flag = true;
	}
}

static void read_object(struct reader *r, const struct web_json_object *schema, void *base);
static void read_field_value(struct reader *r, const struct web_json_field *f, void *base);

static bool in_enum(const char *const *values, const char *value)
{
	for (size_t i = 0; values[i] != NULL; i++) {
		if (strcmp(values[i], value) == 0) {
			return true;
		}
	}
	return false;
}

static void read_string_field(struct reader *r, const struct web_json_field *f, void *base)
{
	char *dst = slot(r, base, f->offset, f->size);
	struct text t = {.dst = dst, .cap = dst != NULL ? f->size : 0U};

	read_string(r, &t);
	if (failed(r)) {
		return;
	}
	if (t.overflow || (f->max_len > 0U && t.chars > f->max_len)) {
		report(r, API_FIELD_TOO_LONG);
	} else if (t.chars < f->min_len) {
		report(r, API_FIELD_OUT_OF_RANGE);
	} else if (t.bad || (f->format != NULL && dst != NULL && !f->format(dst))) {
		report(r, API_FIELD_INVALID_FORMAT);
	} else if (f->enum_values != NULL && dst != NULL && !in_enum(f->enum_values, dst)) {
		report(r, API_FIELD_NOT_ALLOWED);
	} else {
		return;
	}
	/* A rejected value does not stay behind in the struct. */
	if (dst != NULL) {
		memset(dst, 0, f->size);
	}
}

static void read_array_field(struct reader *r, const struct web_json_field *f, void *base)
{
	size_t count = 0;
	void *items = slot(r, base, f->offset, f->item_size * f->max_len);

	r->p++; /* [ */
	skip_ws(r);
	if (peek(r, ']')) {
		r->p++;
	} else {
		for (;;) {
			size_t saved = path_push_index(r, count);

			if (count < f->max_len && items != NULL) {
				read_field_value(r, f->items, (uint8_t *)items + count * f->item_size);
			} else {
				skip_value(r);
			}
			path_pop(r, saved);
			count++;
			if (failed(r)) {
				return;
			}
			skip_ws(r);
			if (peek(r, ',')) {
				r->p++;
				continue;
			}
			if (peek(r, ']')) {
				r->p++;
				break;
			}
			syntax_error(r, "Expected ',' or ']' in an array");
			return;
		}
	}

	if (count > f->max_len || count < f->min_len) {
		report(r, API_FIELD_OUT_OF_RANGE);
	}

	size_t *count_slot = slot(r, base, f->count_offset, sizeof(size_t));

	if (count_slot != NULL) {
		*count_slot = MIN(count, (size_t)f->max_len);
	}
}

static void read_value(struct reader *r, const struct web_json_field *f, void *base);

/*
 * WEB_JSON_ONEOF: whatever went wrong inside the value becomes one
 * `conflicting` report on the value. The reports it would have made are
 * dropped rather than kept beside it, because the mock names only the value
 * for a oneOf no branch accepts.
 */
static void read_field_value(struct reader *r, const struct web_json_field *f, void *base)
{
	const size_t before = r->field_count;
	const bool truncated = r->truncated;

	read_value(r, f, base);
	if ((f->flags & WEB_JSON_ONEOF) && !failed(r) &&
	    (r->field_count != before || r->truncated != truncated)) {
		r->field_count = before;
		r->truncated = truncated;
		report(r, API_FIELD_CONFLICTING);
	}
}

static void read_value(struct reader *r, const struct web_json_field *f, void *base)
{
	skip_ws(r);
	if (r->p >= r->end) {
		syntax_error(r, "Unexpected end of the body");
		return;
	}

	if (*r->p == 'n') {
		if (!read_literal(r, "null")) {
			return;
		}
		if (f->flags & WEB_JSON_NULLABLE) {
			set_flag(r, base, f->null_offset);
		} else {
			report(r, API_FIELD_INVALID_FORMAT);
		}
		return;
	}

	switch (f->type) {
	case WEB_JSON_STRING:
		if (*r->p == '"') {
			read_string_field(r, f, base);
			return;
		}
		break;
	case WEB_JSON_BOOL:
		if (*r->p == 't' || *r->p == 'f') {
			bool value = *r->p == 't';

			if (read_literal(r, value ? "true" : "false")) {
				bool *dst = slot(r, base, f->offset, sizeof(bool));

				if (dst != NULL) {
					*dst = value;
				}
			}
			return;
		}
		break;
	case WEB_JSON_INT:
		if (*r->p == '-' || (*r->p >= '0' && *r->p <= '9')) {
			int64_t value;
			enum number_kind kind = read_number(r, &value);

			if (failed(r)) {
				return;
			}
			if (kind == NUMBER_FRACTIONAL) {
				report(r, API_FIELD_INVALID_FORMAT);
			} else if (kind == NUMBER_OUT_OF_RANGE || value < f->min || value > f->max) {
				report(r, API_FIELD_OUT_OF_RANGE);
			} else {
				int64_t *dst = slot(r, base, f->offset, sizeof(int64_t));

				if (dst != NULL) {
					*dst = value;
				}
			}
			return;
		}
		break;
	case WEB_JSON_OBJECT:
		if (*r->p == '{') {
			if (r->depth >= WEB_JSON_MAX_DEPTH) {
				syntax_error(r, "The body is nested too deeply");
				return;
			}
			r->depth++;
			read_object(r, f->object, (uint8_t *)base + f->offset);
			r->depth--;
			return;
		}
		break;
	case WEB_JSON_ARRAY:
		if (*r->p == '[') {
			if (r->depth >= WEB_JSON_MAX_DEPTH) {
				syntax_error(r, "The body is nested too deeply");
				return;
			}
			r->depth++;
			read_array_field(r, f, base);
			r->depth--;
			return;
		}
		break;
	default:
		break;
	}

	/* The value is not of the declared type: read it for syntax, report it. */
	skip_value(r);
	if (!failed(r)) {
		report(r, API_FIELD_INVALID_FORMAT);
	}
}

static const struct web_json_field *find_field(const struct web_json_object *schema,
					       const struct text *name)
{
	for (size_t i = 0; i < schema->field_count; i++) {
		const struct web_json_field *f = &schema->fields[i];

		if (name->dst != NULL && !name->overflow && strlen(f->name) == name->bytes &&
		    memcmp(f->name, name->dst, name->bytes) == 0) {
			return f;
		}
	}
	return NULL;
}

/* Precondition: *r->p == '{', depth already accounted by the caller. */
static void read_object(struct reader *r, const struct web_json_object *schema, void *base)
{
	struct members members = {0};
	uint64_t seen = 0;
	char name_buf[NAME_MAX + 1];

	__ASSERT(schema->field_count <= 64, "at most 64 members per described object");

	r->p++;
	skip_ws(r);
	if (peek(r, '}')) {
		r->p++;
	} else {
		for (;;) {
			struct text name = {.dst = name_buf, .cap = sizeof(name_buf)};
			const struct web_json_field *f;
			size_t saved;

			skip_ws(r);
			if (!peek(r, '"')) {
				syntax_error(r, "Expected a member name");
				return;
			}
			read_string(r, &name);
			if (failed(r) || member_is_duplicate(r, &members, &name)) {
				return;
			}
			skip_ws(r);
			if (!peek(r, ':')) {
				syntax_error(r, "Expected ':' after a member name");
				return;
			}
			r->p++;

			f = name.bad ? NULL : find_field(schema, &name);
			/* A name too long to keep cannot be a pointer segment either; the
			 * report names the enclosing object. */
			saved = name.overflow ? r->path_len
					      : path_push_bytes(r, name.dst, name.bytes);
			if (f == NULL) {
				skip_value(r);
				if (!failed(r)) {
					report(r, API_FIELD_UNKNOWN);
				}
			} else {
				seen |= BIT64(f - schema->fields);
				if (f->flags & WEB_JSON_PRESENT) {
					set_flag(r, base, f->present_offset);
				}
				read_field_value(r, f, base);
			}
			path_pop(r, saved);
			if (failed(r)) {
				return;
			}

			skip_ws(r);
			if (peek(r, ',')) {
				r->p++;
				continue;
			}
			if (peek(r, '}')) {
				r->p++;
				break;
			}
			syntax_error(r, "Expected ',' or '}' in an object");
			return;
		}
	}

	for (size_t i = 0; i < schema->field_count; i++) {
		const struct web_json_field *f = &schema->fields[i];

		if ((f->flags & WEB_JSON_REQUIRED) && !(seen & BIT64(i))) {
			size_t saved = path_push_bytes(r, f->name, strlen(f->name));

			report(r, API_FIELD_REQUIRED);
			path_pop(r, saved);
		}
	}
}

/* -- ordering ----------------------------------------------------------- */

static bool all_digits(const char *s, size_t n)
{
	if (n == 0U) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
	}
	return true;
}

/* Segment-wise comparison of two pointers; array indices compare as numbers. */
static int path_compare(const char *a, const char *b)
{
	/* "/" is the root, which has no segments. */
	if (strcmp(a, "/") == 0) {
		a = "";
	}
	if (strcmp(b, "/") == 0) {
		b = "";
	}
	while (*a == '/' && *b == '/') {
		const char *ea = strchr(a + 1, '/');
		const char *eb = strchr(b + 1, '/');
		size_t na = ea ? (size_t)(ea - a - 1) : strlen(a + 1);
		size_t nb = eb ? (size_t)(eb - b - 1) : strlen(b + 1);
		int c;

		if (all_digits(a + 1, na) && all_digits(b + 1, nb) && na != nb) {
			c = (na < nb) ? -1 : 1;
		} else {
			c = memcmp(a + 1, b + 1, MIN(na, nb));
			if (c == 0 && na != nb) {
				c = (na < nb) ? -1 : 1;
			}
		}
		if (c != 0) {
			return c;
		}
		a += na + 1U;
		b += nb + 1U;
	}
	if (*a == *b) {
		return 0;
	}
	return (*a == '\0') ? -1 : 1;
}

/* Lower wins when two reports name the same path (json_reader.h). */
static int precedence(enum api_field_code code)
{
	switch (code) {
	case API_FIELD_REQUIRED:
		return 0;
	case API_FIELD_UNKNOWN:
		return 1;
	case API_FIELD_TOO_LONG:
		return 2;
	case API_FIELD_OUT_OF_RANGE:
		return 3;
	case API_FIELD_INVALID_FORMAT:
		return 4;
	case API_FIELD_NOT_ALLOWED:
		return 5;
	default:
		return 6;
	}
}

static void sort_fields(struct reader *r)
{
	size_t out = 0;

	/* Insertion sort: stable, and there are at most MAX_COLLECTED. */
	for (size_t i = 1; i < r->field_count; i++) {
		struct collected tmp = r->fields[i];
		size_t j = i;

		while (j > 0U && path_compare(r->fields[j - 1U].path, tmp.path) > 0) {
			r->fields[j] = r->fields[j - 1U];
			j--;
		}
		r->fields[j] = tmp;
	}

	/* One entry per path: two undeclared names clipped to the same parent
	 * are one report, carrying the code that ranks first. */
	for (size_t i = 0; i < r->field_count; i++) {
		if (out > 0U && strcmp(r->fields[out - 1U].path, r->fields[i].path) == 0) {
			if (precedence(r->fields[i].code) < precedence(r->fields[out - 1U].code)) {
				r->fields[out - 1U].code = r->fields[i].code;
			}
			continue;
		}
		r->fields[out++] = r->fields[i];
	}
	r->field_count = out;
}

/* -- entry point -------------------------------------------------------- */

/*
 * The reader is a few kilobytes and lives in static storage rather than on the
 * HTTP server's stack. That makes decoding single-threaded, which it is: every
 * request is decoded on the server's one thread.
 */
int web_json_decode(const char *body, size_t len, const struct web_json_object *schema,
		    void *dest, size_t dest_size, struct api_error *err)
{
	static struct reader reader;
	struct reader *r = &reader;

	memset(r, 0, sizeof(*r));
	r->p = (const uint8_t *)body;
	r->end = (const uint8_t *)body + len;
	r->dest_base = dest;
	r->dest_size = dest_size;
	memset(dest, 0, dest_size);

	skip_ws(r);
	if (peek(r, '{')) {
		r->depth = 1;
		read_object(r, schema, dest);
		r->depth = 0;
	} else {
		skip_value(r);
		if (!failed(r)) {
			report(r, API_FIELD_INVALID_FORMAT);
		}
	}
	if (!failed(r)) {
		skip_ws(r);
		if (r->p != r->end) {
			syntax_error(r, "Unexpected data after the JSON value");
		}
	}

	if (failed(r)) {
		(void)api_error_init(err, API_ERR_INVALID_JSON, r->syntax, NULL);
		memset(dest, 0, dest_size);
		return -EBADMSG;
	}
	if (r->field_count == 0U) {
		return 0;
	}

	/* A rejected body leaves nothing behind: it may have held a password. */
	memset(dest, 0, dest_size);
	sort_fields(r);
	(void)api_error_init(err, API_ERR_VALIDATION_FAILED, "The request body is not acceptable",
			     NULL);
	for (size_t i = 0; i < r->field_count; i++) {
		(void)api_error_add_field(err, r->fields[i].path, r->fields[i].code);
	}
	if (r->truncated) {
		err->fields_truncated = true;
	}

	return -EINVAL;
}
