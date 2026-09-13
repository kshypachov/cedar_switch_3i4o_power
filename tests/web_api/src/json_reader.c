/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Strict request decoding: every syntax refusal is invalid_json, every schema
 * refusal names its field, and the field list is the same one the mock
 * produces for the same body.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <web_api/json_reader.h>

struct inner {
	int64_t level;
	bool on;
};

struct item {
	char label[9];
};

struct doc {
	char name[17];
	bool name_present;
	char note[9];
	bool note_null;
	int64_t count;
	bool flag;
	char mode[8];
	char addr[16];
	struct inner inner;
	struct item items[3];
	size_t item_count;
	char tiny[4]; /* 3 bytes of text, but up to 8 code points by the schema */
};

static const char *const modes[] = {"dhcp", "static", NULL};

static bool is_dotted(const char *v)
{
	return strchr(v, '.') != NULL;
}

static const struct web_json_field inner_fields[] = {
	{.name = "level", .type = WEB_JSON_INT, .flags = WEB_JSON_REQUIRED,
	 .offset = offsetof(struct inner, level), .min = -5, .max = 5},
	{.name = "on", .type = WEB_JSON_BOOL, .offset = offsetof(struct inner, on)},
};
static const struct web_json_object inner_schema = {inner_fields, ARRAY_SIZE(inner_fields)};

static const struct web_json_field item_field = {
	.type = WEB_JSON_STRING, .offset = offsetof(struct item, label), .size = 9, .max_len = 8,
};

static const struct web_json_field doc_fields[] = {
	{.name = "name", .type = WEB_JSON_STRING, .flags = WEB_JSON_REQUIRED | WEB_JSON_PRESENT,
	 .offset = offsetof(struct doc, name), .present_offset = offsetof(struct doc, name_present),
	 .size = 17, .min_len = 2, .max_len = 16},
	{.name = "note", .type = WEB_JSON_STRING, .flags = WEB_JSON_NULLABLE,
	 .offset = offsetof(struct doc, note), .null_offset = offsetof(struct doc, note_null),
	 .size = 9, .max_len = 8},
	{.name = "count", .type = WEB_JSON_INT, .offset = offsetof(struct doc, count),
	 .min = INT64_MIN, .max = 1000},
	{.name = "flag", .type = WEB_JSON_BOOL, .offset = offsetof(struct doc, flag)},
	{.name = "mode", .type = WEB_JSON_STRING, .offset = offsetof(struct doc, mode), .size = 8,
	 .max_len = 7, .enum_values = modes},
	{.name = "addr", .type = WEB_JSON_STRING, .offset = offsetof(struct doc, addr), .size = 16,
	 .max_len = 15, .format = is_dotted},
	{.name = "inner", .type = WEB_JSON_OBJECT, .offset = offsetof(struct doc, inner),
	 .object = &inner_schema},
	{.name = "items", .type = WEB_JSON_ARRAY, .offset = offsetof(struct doc, items),
	 .items = &item_field, .item_size = sizeof(struct item),
	 .count_offset = offsetof(struct doc, item_count), .min_len = 1, .max_len = 3},
	{.name = "tiny", .type = WEB_JSON_STRING, .offset = offsetof(struct doc, tiny), .size = 4,
	 .max_len = 8},
};
static const struct web_json_object doc_schema = {doc_fields, ARRAY_SIZE(doc_fields)};

static struct doc d;
static struct api_error err;

static int decode(const char *json)
{
	memset(&err, 0, sizeof(err));
	return web_json_decode(json, strlen(json), &doc_schema, &d, sizeof(d), &err);
}

static void expect_syntax(const char *json)
{
	zassert_equal(decode(json), -EBADMSG, "should be invalid_json: %s", json);
	zassert_equal(err.code, API_ERR_INVALID_JSON, "%s", json);
	zassert_equal(err.field_count, 0);
}

/* Expect validation_failed with exactly these path=code pairs, in order. */
static void expect_fields(const char *json, size_t n, const char *const pairs[][2])
{
	zassert_equal(decode(json), -EINVAL, "should be validation_failed: %s", json);
	zassert_equal(err.code, API_ERR_VALIDATION_FAILED);
	zassert_equal(err.field_count, n, "%s: %u fields", json, err.field_count);
	for (size_t i = 0; i < n; i++) {
		zassert_str_equal(err.fields[i].path, pairs[i][0], "%s: field %zu", json, i);
		zassert_str_equal(api_field_code_str(err.fields[i].code), pairs[i][1],
				  "%s: field %zu (%s)", json, i, pairs[i][0]);
	}
}

#define FIELDS(...) ((const char *const[][2]){__VA_ARGS__})
#define EXPECT_FIELDS(json, ...)                                                                   \
	expect_fields(json, sizeof(FIELDS(__VA_ARGS__)) / sizeof(FIELDS(__VA_ARGS__)[0]),          \
		      FIELDS(__VA_ARGS__))

ZTEST_SUITE(json_reader, NULL, NULL, NULL, NULL, NULL);

ZTEST(json_reader, test_full_document)
{
	zassert_ok(decode("{\"name\":\"ab\",\"note\":\"n\",\"count\":-7,\"flag\":true,"
			  "\"mode\":\"static\",\"addr\":\"1.2\",\"inner\":{\"level\":5,\"on\":true},"
			  "\"items\":[\"x\",\"yy\"],\"tiny\":\"abc\"}"));
	zassert_str_equal(d.name, "ab");
	zassert_true(d.name_present);
	zassert_str_equal(d.note, "n");
	zassert_false(d.note_null);
	zassert_equal(d.count, -7);
	zassert_true(d.flag);
	zassert_str_equal(d.mode, "static");
	zassert_str_equal(d.addr, "1.2");
	zassert_equal(d.inner.level, 5);
	zassert_true(d.inner.on);
	zassert_equal(d.item_count, 2);
	zassert_str_equal(d.items[0].label, "x");
	zassert_str_equal(d.items[1].label, "yy");
	zassert_str_equal(d.tiny, "abc");
}

ZTEST(json_reader, test_absent_members_read_as_zero)
{
	memset(&d, 0x5A, sizeof(d));
	zassert_ok(decode(" {\"name\" : \"ab\"}\r\n\t "));
	zassert_equal(d.count, 0);
	zassert_false(d.flag);
	zassert_equal(d.note[0], '\0');
	zassert_equal(d.item_count, 0);
	zassert_false(d.note_null);
}

ZTEST(json_reader, test_null)
{
	zassert_ok(decode("{\"name\":\"ab\",\"note\":null}"));
	zassert_true(d.note_null);
	EXPECT_FIELDS("{\"name\":null}", {"/name", "invalid_format"});
	EXPECT_FIELDS("{\"name\":\"ab\",\"count\":null}", {"/count", "invalid_format"});
}

ZTEST(json_reader, test_present_flag_is_opt_in)
{
	/* "note" has no WEB_JSON_PRESENT; its zero present_offset must not have
	 * written into the first byte of "name". */
	zassert_ok(decode("{\"note\":\"x\",\"name\":\"ab\"}"));
	zassert_str_equal(d.name, "ab");
}

ZTEST(json_reader, test_syntax_refusals)
{
	static const char *const bad[] = {
		"",
		"   ",
		"{",
		"}",
		"{\"name\":\"ab\"",
		"{\"name\":\"ab\",}",
		"{,\"name\":\"ab\"}",
		"{\"name\" \"ab\"}",
		"{name:\"ab\"}",
		"{'name':'ab'}",
		"{\"name\":\"ab\"} x",
		"{\"name\":\"ab\"}{}",
		"{\"count\":01}",
		"{\"count\":-}",
		"{\"count\":1.}",
		"{\"count\":.5}",
		"{\"count\":1e}",
		"{\"count\":+1}",
		"{\"count\":0x10}",
		"{\"flag\":tru}",
		"{\"flag\":True}",
		"{\"note\":nul}",
		"{\"name\":\"a\nb\"}",
		"{\"name\":\"a\\qb\"}",
		"{\"name\":\"a\\u12G4\"}",
		"{\"name\":\"a\\u12\"}",
		"{\"name\":\"ab}",
		"{\"items\":[\"a\",]}",
		"{\"items\":[\"a\" \"b\"]}",
		"[1,2",
	};

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		expect_syntax(bad[i]);
	}
}

ZTEST(json_reader, test_invalid_utf8_is_syntax)
{
	static const char *const bad[] = {
		"{\"name\":\"\xC0\xAF\"}",     /* overlong */
		"{\"name\":\"\x80\"}",         /* lone continuation */
		"{\"name\":\"\xE2\x82\"}",     /* truncated */
		"{\"name\":\"\xED\xA0\x80\"}", /* encoded surrogate */
		"{\"name\":\"\xF4\x90\x80\x80\"}", /* past U+10FFFF */
		"{\"name\":\"\xFF\"}",
		"{\"\xC0\xAF\":1}",
	};

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		expect_syntax(bad[i]);
	}
}

ZTEST(json_reader, test_duplicate_names_are_syntax)
{
	expect_syntax("{\"name\":\"ab\",\"name\":\"cd\"}");
	expect_syntax("{\"name\":\"ab\",\"x\":1,\"x\":2}");
	expect_syntax("{\"name\":\"ab\",\"inner\":{\"level\":1,\"level\":2}}");
	expect_syntax("{\"name\":\"ab\",\"x\":{\"a\":1,\"a\":1}}");
	/* Spelled differently, the same name. */
	expect_syntax("{\"name\":\"ab\",\"n\\u0061me\":\"cd\"}");
}

ZTEST(json_reader, test_syntax_error_wins_over_schema_errors)
{
	/* Schema problems first, then a break: still invalid_json. */
	expect_syntax("{\"extra\":1,\"count\":\"x\",\"name\":");
	zassert_equal(d.count, 0, "nothing is left in the destination");
}

ZTEST(json_reader, test_depth)
{
	char deep[64] = "{\"name\":\"ab\",\"x\":";
	size_t n = strlen(deep);

	/* The document is level 1; seven more fit, the eighth does not. */
	for (int i = 0; i < WEB_JSON_MAX_DEPTH - 1; i++) {
		deep[n++] = '[';
	}
	for (int i = 0; i < WEB_JSON_MAX_DEPTH - 1; i++) {
		deep[n++] = ']';
	}
	deep[n++] = '}';
	deep[n] = '\0';
	EXPECT_FIELDS(deep, {"/x", "unknown_field"});

	strcpy(deep, "{\"name\":\"ab\",\"x\":");
	n = strlen(deep);
	for (int i = 0; i < WEB_JSON_MAX_DEPTH; i++) {
		deep[n++] = '[';
	}
	for (int i = 0; i < WEB_JSON_MAX_DEPTH; i++) {
		deep[n++] = ']';
	}
	deep[n++] = '}';
	deep[n] = '\0';
	expect_syntax(deep);
}

ZTEST(json_reader, test_top_level_must_be_an_object)
{
	EXPECT_FIELDS("[]", {"/", "invalid_format"});
	EXPECT_FIELDS("null", {"/", "invalid_format"});
	EXPECT_FIELDS(" \"x\" ", {"/", "invalid_format"});
	EXPECT_FIELDS("12", {"/", "invalid_format"});
}

ZTEST(json_reader, test_required_and_unknown)
{
	EXPECT_FIELDS("{}", {"/name", "required"});
	EXPECT_FIELDS("{\"zeta\":1,\"name\":\"ab\",\"alpha\":{\"a\":[1,2]}}",
		      {"/alpha", "unknown_field"}, {"/zeta", "unknown_field"});
	EXPECT_FIELDS("{\"name\":\"ab\",\"inner\":{\"x\":true}}", {"/inner/level", "required"},
		      {"/inner/x", "unknown_field"});
}

ZTEST(json_reader, test_wrong_types)
{
	EXPECT_FIELDS("{\"name\":1,\"count\":\"1\",\"flag\":0,\"inner\":[],\"items\":{},\"mode\":true}",
		      {"/count", "invalid_format"}, {"/flag", "invalid_format"},
		      {"/inner", "invalid_format"}, {"/items", "invalid_format"},
		      {"/mode", "invalid_format"}, {"/name", "invalid_format"});
}

ZTEST(json_reader, test_string_lengths_are_code_points)
{
	/* 16 Cyrillic letters: 32 bytes, 16 code points - fits a 17-byte buffer?
	 * No: the buffer is bytes. "name" is 17 bytes, so this is too_long by
	 * buffer even though it is within max_len by code points. */
	EXPECT_FIELDS("{\"name\":\"абвгдежзийклмноп\"}", {"/name", "too_long"});
	/* 8 Cyrillic letters: 16 bytes, fits. */
	zassert_ok(decode("{\"name\":\"абвгдежз\"}"));
	zassert_str_equal(d.name, "абвгдежз");
	/* 17 ASCII: too long by code points. */
	EXPECT_FIELDS("{\"name\":\"abcdefghijklmnopq\"}", {"/name", "too_long"});
	/* One code point: below min_len 2. A four-byte character is still one. */
	EXPECT_FIELDS("{\"name\":\"\xF0\x9F\x94\x91\"}", {"/name", "out_of_range"});
	zassert_ok(decode("{\"name\":\"\xF0\x9F\x94\x91\xF0\x9F\x94\x91\"}"));
	/* "tiny": within max_len by code points but not by its 4-byte buffer. */
	EXPECT_FIELDS("{\"name\":\"ab\",\"tiny\":\"abcd\"}", {"/tiny", "too_long"});
}

ZTEST(json_reader, test_escapes_decode)
{
	zassert_ok(decode("{\"name\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}"));
	zassert_mem_equal(d.name, "\"\\/\b\f\n\r\t", 8);
	zassert_ok(decode("{\"name\":\"\\u00e9\\u20AC\\ud83d\\ude00\"}"));
	zassert_str_equal(d.name, "é€\xF0\x9F\x98\x80");
}

ZTEST(json_reader, test_text_c_cannot_hold)
{
	EXPECT_FIELDS("{\"name\":\"a\\u0000b\"}", {"/name", "invalid_format"});
	EXPECT_FIELDS("{\"name\":\"ab\\ud800\"}", {"/name", "invalid_format"});
	EXPECT_FIELDS("{\"name\":\"ab\\udc00x\"}", {"/name", "invalid_format"});
	EXPECT_FIELDS("{\"name\":\"a\\ud800\\u0041\"}", {"/name", "invalid_format"});
}

ZTEST(json_reader, test_enum_and_format)
{
	EXPECT_FIELDS("{\"name\":\"ab\",\"mode\":\"auto\"}", {"/mode", "not_allowed"});
	EXPECT_FIELDS("{\"name\":\"ab\",\"mode\":\"DHCP\"}", {"/mode", "not_allowed"});
	EXPECT_FIELDS("{\"name\":\"ab\",\"addr\":\"nodots\"}", {"/addr", "invalid_format"});
	/* Precedence: too_long before format. */
	EXPECT_FIELDS("{\"name\":\"ab\",\"addr\":\"nodotsnodotsnodots\"}", {"/addr", "too_long"});
}

ZTEST(json_reader, test_integers_are_values)
{
	static const struct {
		const char *text;
		int64_t value;
	} ok[] = {
		{"0", 0},       {"-0", 0},     {"120", 120},     {"120.0", 120},
		{"1.2e2", 120}, {"12E1", 120}, {"1200e-1", 120}, {"0.0e5", 0},
		{"-9223372036854775808", INT64_MIN}, {"1000", 1000}, {"100e-2", 1},
		{"0.000", 0},
	};
	static const char *const fractional[] = {"1.5", "12e-1", "0.001", "-0.5"};
	static const char *const range[] = {"1001", "9223372036854775808", "1e19", "1e100000",
					    "-9223372036854775809"};
	char json[80];

	for (size_t i = 0; i < ARRAY_SIZE(ok); i++) {
		snprintf(json, sizeof(json), "{\"name\":\"ab\",\"count\":%s}", ok[i].text);
		zassert_ok(decode(json), "%s", json);
		zassert_equal(d.count, ok[i].value, "%s", ok[i].text);
	}
	for (size_t i = 0; i < ARRAY_SIZE(fractional); i++) {
		snprintf(json, sizeof(json), "{\"name\":\"ab\",\"count\":%s}", fractional[i]);
		EXPECT_FIELDS(json, {"/count", "invalid_format"});
	}
	for (size_t i = 0; i < ARRAY_SIZE(range); i++) {
		snprintf(json, sizeof(json), "{\"name\":\"ab\",\"count\":%s}", range[i]);
		EXPECT_FIELDS(json, {"/count", "out_of_range"});
	}
	EXPECT_FIELDS("{\"name\":\"ab\",\"inner\":{\"level\":-6}}", {"/inner/level", "out_of_range"});
}

/*
 * The two oneOf values of the network request, CredentialChange and a DNS
 * server: the mock names the value, not what is wrong inside it
 * (tools/api-contract/tests/test_mock_network.py, the malformed credential and
 * the resolver that is not an address).
 */
struct credential {
	char action[9];
	char value[9];
	bool value_present;
};

struct oneof_doc {
	struct credential cred;
	char servers[2][8];
	size_t server_count;
	int64_t other;
};

static const char *const actions[] = {"keep", "replace", "clear", NULL};

static const struct web_json_field credential_fields[] = {
	{.name = "action", .type = WEB_JSON_STRING, .flags = WEB_JSON_REQUIRED,
	 .offset = offsetof(struct credential, action), .size = 9, .max_len = 8,
	 .enum_values = actions},
	{.name = "value", .type = WEB_JSON_STRING, .flags = WEB_JSON_PRESENT,
	 .offset = offsetof(struct credential, value),
	 .present_offset = offsetof(struct credential, value_present), .size = 9, .min_len = 1,
	 .max_len = 8},
};
static const struct web_json_object credential_schema = {credential_fields,
							  ARRAY_SIZE(credential_fields)};

static const struct web_json_field server_field = {
	.type = WEB_JSON_STRING, .flags = WEB_JSON_ONEOF, .offset = 0, .size = 8, .max_len = 7,
	.format = is_dotted,
};

static const struct web_json_field oneof_fields[] = {
	{.name = "cred", .type = WEB_JSON_OBJECT, .flags = WEB_JSON_ONEOF,
	 .offset = offsetof(struct oneof_doc, cred), .object = &credential_schema},
	{.name = "servers", .type = WEB_JSON_ARRAY, .offset = offsetof(struct oneof_doc, servers),
	 .items = &server_field, .item_size = 8,
	 .count_offset = offsetof(struct oneof_doc, server_count), .max_len = 2},
	{.name = "other", .type = WEB_JSON_INT, .offset = offsetof(struct oneof_doc, other),
	 .min = 0, .max = 9},
};
static const struct web_json_object oneof_schema = {oneof_fields, ARRAY_SIZE(oneof_fields)};

static void expect_oneof(const char *json, size_t n, const char *const pairs[][2])
{
	struct oneof_doc o;

	memset(&err, 0, sizeof(err));
	zassert_equal(web_json_decode(json, strlen(json), &oneof_schema, &o, sizeof(o), &err),
		      -EINVAL, "%s", json);
	zassert_equal(err.field_count, n, "%s: %u fields", json, err.field_count);
	for (size_t i = 0; i < n; i++) {
		zassert_str_equal(err.fields[i].path, pairs[i][0], "%s: field %zu", json, i);
		zassert_str_equal(api_field_code_str(err.fields[i].code), pairs[i][1],
				  "%s: field %zu (%s)", json, i, pairs[i][0]);
	}
}

#define EXPECT_ONEOF(json, ...)                                                                    \
	expect_oneof(json, sizeof(FIELDS(__VA_ARGS__)) / sizeof(FIELDS(__VA_ARGS__)[0]),           \
		     FIELDS(__VA_ARGS__))

ZTEST(json_reader, test_oneof_values_are_one_conflicting_entry)
{
	struct oneof_doc o;
	const char *ok = "{\"cred\":{\"action\":\"replace\",\"value\":\"secret\"},"
			 "\"servers\":[\"1.2\",\"3.4\"]}";

	memset(&err, 0, sizeof(err));
	zassert_ok(web_json_decode(ok, strlen(ok), &oneof_schema, &o, sizeof(o), &err));
	zassert_str_equal(o.cred.value, "secret");
	zassert_true(o.cred.value_present);
	zassert_equal(o.server_count, 2);

	EXPECT_ONEOF("{\"cred\":{}}", {"/cred", "conflicting"});
	EXPECT_ONEOF("{\"cred\":{\"action\":\"sometimes\"}}", {"/cred", "conflicting"});
	EXPECT_ONEOF("{\"cred\":{\"action\":\"keep\",\"stray\":1}}", {"/cred", "conflicting"});
	EXPECT_ONEOF("{\"cred\":{\"action\":\"replace\",\"value\":\"much too long\"}}",
		     {"/cred", "conflicting"});
	EXPECT_ONEOF("{\"cred\":\"keep\"}", {"/cred", "conflicting"});
	EXPECT_ONEOF("{\"cred\":null}", {"/cred", "conflicting"});
	EXPECT_ONEOF("{\"servers\":[\"name\",53]}", {"/servers/0", "conflicting"},
		     {"/servers/1", "conflicting"});

	/* Only the oneOf value collapses; its neighbours report as usual. */
	EXPECT_ONEOF("{\"cred\":{\"x\":1},\"other\":10,\"extra\":1}", {"/cred", "conflicting"},
		     {"/extra", "unknown_field"}, {"/other", "out_of_range"});

	/* A syntax error inside is still a syntax error. */
	memset(&err, 0, sizeof(err));
	zassert_equal(web_json_decode("{\"cred\":{\"action\":}", 19, &oneof_schema, &o, sizeof(o),
				      &err),
		      -EBADMSG);
}

/*
 * What a oneOf value would have reported goes entirely, including the fact that
 * there was too much of it to collect: the value is one conflicting entry and
 * the list is complete. Found by mutation: no oneOf value tested produced more
 * reports than the reader collects, so keeping the truncation broke nothing.
 */
ZTEST(json_reader, test_a_oneof_value_does_not_leave_truncation_behind)
{
	struct oneof_doc o;
	char json[512];
	size_t pos = 0;

	/* 40 unknown members inside the credential: more than the 32 collected. */
	pos += snprintf(json + pos, sizeof(json) - pos, "{\"cred\":{");
	for (int i = 0; i < 40; i++) {
		pos += snprintf(json + pos, sizeof(json) - pos, "%s\"k%02d\":1", i ? "," : "", i);
	}
	pos += snprintf(json + pos, sizeof(json) - pos, "}}");
	zassert_true(pos < sizeof(json));

	memset(&err, 0, sizeof(err));
	zassert_equal(web_json_decode(json, pos, &oneof_schema, &o, sizeof(o), &err), -EINVAL);
	zassert_equal(err.field_count, 1);
	zassert_str_equal(err.fields[0].path, "/cred");
	zassert_equal(err.fields[0].code, API_FIELD_CONFLICTING);
	zassert_false(err.fields_truncated, "one entry is the whole list");
}

ZTEST(json_reader, test_arrays)
{
	EXPECT_FIELDS("{\"name\":\"ab\",\"items\":[]}", {"/items", "out_of_range"});
	EXPECT_FIELDS("{\"name\":\"ab\",\"items\":[\"a\",\"b\",\"c\",\"d\"]}",
		      {"/items", "out_of_range"});
	zassert_equal(d.item_count, 0, "nothing kept from a rejected body");

	EXPECT_FIELDS("{\"name\":\"ab\",\"items\":[\"a\",1,\"toolongvalue\"]}",
		      {"/items/1", "invalid_format"}, {"/items/2", "too_long"});
}

ZTEST(json_reader, test_fields_sort_by_path_segments)
{
	/* 11 elements in a 3-element array: only the first three are decoded,
	 * but ordering must put /items/10 after /items/2 - here we use nested
	 * member names to check segment-wise comparison instead. */
	EXPECT_FIELDS("{\"name\":\"ab\",\"inner\":{\"on\":1,\"level\":\"x\"},\"count\":\"x\","
		      "\"aa\":1,\"inn\":1}",
		      {"/aa", "unknown_field"}, {"/count", "invalid_format"},
		      {"/inn", "unknown_field"}, {"/inner/level", "invalid_format"},
		      {"/inner/on", "invalid_format"});
}

ZTEST(json_reader, test_numeric_segments_compare_as_numbers)
{
	static const struct web_json_field many_item = {
		.type = WEB_JSON_BOOL, .offset = 0,
	};
	struct many {
		bool v[12];
		size_t n;
	} m;
	const struct web_json_field many_fields[] = {
		{.name = "v", .type = WEB_JSON_ARRAY, .offset = offsetof(struct many, v),
		 .items = &many_item, .item_size = sizeof(bool),
		 .count_offset = offsetof(struct many, n), .max_len = 12},
	};
	const struct web_json_object many_schema = {many_fields, 1};
	const char *json = "{\"v\":[true,true,1,true,true,true,true,true,true,true,1,true]}";

	zassert_equal(web_json_decode(json, strlen(json), &many_schema, &m, sizeof(m), &err),
		      -EINVAL);
	zassert_equal(err.field_count, 2);
	zassert_str_equal(err.fields[0].path, "/v/2");
	zassert_str_equal(err.fields[1].path, "/v/10");
}

ZTEST(json_reader, test_truncation)
{
	zassert_equal(decode("{\"a\":1,\"b\":1,\"c\":1,\"d\":1,\"e\":1,\"f\":1,\"g\":1}"), -EINVAL);
	zassert_equal(err.field_count, CONFIG_API_VALIDATION_MAX_FIELDS);
	zassert_true(err.fields_truncated);
	/* Sorted before truncating: the survivors are the first by path. */
	zassert_str_equal(err.fields[0].path, "/a");
	zassert_str_equal(err.fields[5].path, "/f");
}

ZTEST(json_reader, test_pointer_escaping)
{
	EXPECT_FIELDS("{\"name\":\"ab\",\"a/b~c\":1}", {"/a~1b~0c", "unknown_field"});
	/* A name too long for a pointer is reported at its parent. */
	EXPECT_FIELDS("{\"name\":\"ab\",\"inner\":{\"level\":1,"
		      "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\":1}}",
		      {"/inner", "unknown_field"});
	/* So is one with a control character, which a path may not contain. */
	EXPECT_FIELDS("{\"name\":\"ab\",\"x\\u0001\":1}", {"/", "unknown_field"});
	/* Two such names are one report at the parent, not two. */
	EXPECT_FIELDS("{\"name\":\"ab\",\"x\\u0001\":1,\"y\\u0002\":1}", {"/", "unknown_field"});
	/* A name longer than the decoder keeps is also reported at its parent,
	 * not at an empty segment. */
	EXPECT_FIELDS("{\"name\":\"ab\",\"inner\":{\"level\":1,\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\":1}}",
		      {"/inner", "unknown_field"});
}

ZTEST(json_reader, test_rejected_value_does_not_stay)
{
	zassert_equal(decode("{\"name\":\"ab\",\"mode\":\"auto\"}"), -EINVAL);
	zassert_equal(d.mode[0], '\0');
}

ZTEST(json_reader, test_reads_only_the_given_length)
{
	const char json[] = "{\"name\":\"ab\"}GARBAGE";

	zassert_ok(web_json_decode(json, 13, &doc_schema, &d, sizeof(d), &err));
}

ZTEST(json_reader, test_code_point_limit_is_checked_where_the_buffer_has_room)
{
	/* A password-like field: the buffer holds 4 bytes per character, so a
	 * value one character too long still fits and only the count refuses it. */
	struct roomy {
		char text[33];
	} v;
	const struct web_json_field fields[] = {
		{.name = "text", .type = WEB_JSON_STRING, .offset = offsetof(struct roomy, text),
		 .size = sizeof(v.text), .max_len = 8},
	};
	const struct web_json_object schema = {fields, 1};
	const char *ok = "{\"text\":\"abcdefgh\"}";
	const char *over = "{\"text\":\"abcdefghi\"}";

	zassert_ok(web_json_decode(ok, strlen(ok), &schema, &v, sizeof(v), &err));
	zassert_equal(web_json_decode(over, strlen(over), &schema, &v, sizeof(v), &err), -EINVAL);
	zassert_str_equal(err.fields[0].path, "/text");
	zassert_equal(err.fields[0].code, API_FIELD_TOO_LONG);
}
