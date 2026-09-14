/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The JSON writer: exact output, escaping through api-validation, and every
 * way a document can be malformed or overflow being reported at finish().
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <web_api/json_writer.h>

static char buf[256];
static struct web_json_writer w;

static void before(void *f)
{
	ARG_UNUSED(f);
	memset(buf, 0x55, sizeof(buf));
	web_json_writer_init(&w, buf, sizeof(buf));
}

ZTEST_SUITE(json_writer, NULL, NULL, before, NULL, NULL);

ZTEST(json_writer, test_nested_document)
{
	web_json_object_begin(&w);
	web_json_key(&w, "a");
	web_json_int(&w, -12);
	web_json_key(&w, "b");
	web_json_array_begin(&w);
	web_json_bool(&w, true);
	web_json_null(&w);
	web_json_object_begin(&w);
	web_json_object_end(&w);
	web_json_array_begin(&w);
	web_json_array_end(&w);
	web_json_string(&w, "x");
	web_json_array_end(&w);
	web_json_key(&w, "c");
	web_json_decimal(&w, 18446744073709551615ULL);
	web_json_key(&w, "d");
	web_json_string_or_null(&w, NULL);
	web_json_key(&w, "e");
	web_json_string_or_null(&w, "y");
	web_json_object_end(&w);

	int len = web_json_writer_finish(&w);

	zassert_str_equal(buf, "{\"a\":-12,\"b\":[true,null,{},[],\"x\"],"
			       "\"c\":\"18446744073709551615\",\"d\":null,\"e\":\"y\"}");
	zassert_equal(len, (int)strlen(buf));
}

ZTEST(json_writer, test_extremes)
{
	web_json_array_begin(&w);
	web_json_int(&w, INT64_MIN);
	web_json_int(&w, INT64_MAX);
	web_json_decimal(&w, 0);
	web_json_bool(&w, false);
	web_json_array_end(&w);
	zassert_true(web_json_writer_finish(&w) > 0);
	zassert_str_equal(buf, "[-9223372036854775808,9223372036854775807,\"0\",false]");
}

ZTEST(json_writer, test_text_is_escaped_like_the_error_body)
{
	web_json_string(&w, "q\"b\\n\nr\rt\tc\x01 кириллица");
	zassert_true(web_json_writer_finish(&w) > 0);
	zassert_str_equal(buf, "\"q\\\"b\\\\n\\nr\\rt\\tc\\u0001 кириллица\"");
}

ZTEST(json_writer, test_bare_scalar_document)
{
	web_json_string(&w, "only");
	zassert_true(web_json_writer_finish(&w) > 0);
	zassert_str_equal(buf, "\"only\"");
}

ZTEST(json_writer, test_structural_errors)
{
	/* A second value at the top. */
	web_json_null(&w);
	web_json_null(&w);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);

	/* A value in an object without a name. */
	web_json_writer_init(&w, buf, sizeof(buf));
	web_json_object_begin(&w);
	web_json_int(&w, 1);
	web_json_object_end(&w);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);

	/* A name inside an array. */
	web_json_writer_init(&w, buf, sizeof(buf));
	web_json_array_begin(&w);
	web_json_key(&w, "k");
	web_json_array_end(&w);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);

	/* A name with no value. */
	web_json_writer_init(&w, buf, sizeof(buf));
	web_json_object_begin(&w);
	web_json_key(&w, "k");
	web_json_object_end(&w);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);

	/* Two names in a row. */
	web_json_writer_init(&w, buf, sizeof(buf));
	web_json_object_begin(&w);
	web_json_key(&w, "k");
	web_json_key(&w, "l");
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);

	/* Unclosed, and mismatched. */
	web_json_writer_init(&w, buf, sizeof(buf));
	web_json_object_begin(&w);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);
	web_json_writer_init(&w, buf, sizeof(buf));
	web_json_object_begin(&w);
	web_json_array_end(&w);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);

	/* Closing what was never opened. */
	web_json_writer_init(&w, buf, sizeof(buf));
	web_json_object_end(&w);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);

	/* Nothing at all. */
	web_json_writer_init(&w, buf, sizeof(buf));
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);

	/* A NULL string. */
	web_json_writer_init(&w, buf, sizeof(buf));
	web_json_string(&w, NULL);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);
}

ZTEST(json_writer, test_depth_limit)
{
	for (int i = 0; i < WEB_JSON_WRITER_MAX_DEPTH; i++) {
		web_json_array_begin(&w);
	}
	for (int i = 0; i < WEB_JSON_WRITER_MAX_DEPTH; i++) {
		web_json_array_end(&w);
	}
	zassert_true(web_json_writer_finish(&w) > 0, "the limit itself is allowed");

	web_json_writer_init(&w, buf, sizeof(buf));
	for (int i = 0; i <= WEB_JSON_WRITER_MAX_DEPTH; i++) {
		web_json_array_begin(&w);
	}
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);
}

ZTEST(json_writer, test_overflow_is_sticky_and_never_overruns)
{
	char small[12];

	memset(small, 0x55, sizeof(small));
	web_json_writer_init(&w, small, sizeof(small) - 2); /* two guard bytes */
	web_json_array_begin(&w);
	web_json_string(&w, "0123456789");
	web_json_int(&w, 1); /* after the failure: ignored */
	web_json_array_end(&w);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);
	zassert_equal((uint8_t)small[sizeof(small) - 1], 0x55, "guard byte");
	zassert_equal((uint8_t)small[sizeof(small) - 2], 0x55, "guard byte");

	/* Exactly fitting, including the terminating NUL. */
	web_json_writer_init(&w, small, 5);
	web_json_string(&w, "ab");
	zassert_equal(web_json_writer_finish(&w), 4);
	zassert_str_equal(small, "\"ab\"");

	web_json_writer_init(&w, small, 4);
	web_json_string(&w, "ab");
	zassert_equal(web_json_writer_finish(&w), -ENOMEM, "no room for the NUL");

	web_json_writer_init(&w, NULL, 0);
	web_json_null(&w);
	zassert_equal(web_json_writer_finish(&w), -ENOMEM);
}

ZTEST(json_writer, test_room_and_rollback)
{
	struct web_json_writer saved;

	zassert_equal(web_json_writer_room(&w), sizeof(buf) - 1, "all but the NUL");
	web_json_array_begin(&w);
	zassert_equal(web_json_writer_room(&w), sizeof(buf) - 2);

	saved = w;
	web_json_string(&w, "first");
	zassert_equal(web_json_writer_room(&w), sizeof(buf) - 2 - 7);
	web_json_writer_rollback(&w, &saved);
	zassert_equal(web_json_writer_room(&w), sizeof(buf) - 2);
	zassert_str_equal(buf, "[", "discarded text is gone, terminated where it stood");

	/* A failure after the copy is forgotten with it. */
	char big[300];

	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	web_json_string(&w, big);
	zassert_equal(web_json_writer_room(&w), 0, "a failed writer has no room");
	web_json_writer_rollback(&w, &saved);
	web_json_string(&w, "ok");
	web_json_array_end(&w);
	zassert_equal(web_json_writer_finish(&w), 6);
	zassert_str_equal(buf, "[\"ok\"]");
	zassert_str_equal(buf, "[\"ok\"]", "no comma from the discarded element");
}
