/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The request pipeline, on a router made for the test: every step of the
 * order in web_api.h, and the helpers it rests on.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "harness.h"

/* -- a router that exercises the middleware without the real services ---- */

struct thing_body {
	char name[17];
	int64_t count;
};

static const struct web_json_field thing_fields[] = {
	{.name = "name", .type = WEB_JSON_STRING, .flags = WEB_JSON_REQUIRED,
	 .offset = offsetof(struct thing_body, name), .size = 17, .min_len = 1, .max_len = 16},
	{.name = "count", .type = WEB_JSON_INT, .offset = offsetof(struct thing_body, count),
	 .min = 0, .max = 10},
};

static const struct web_json_object thing_schema = {thing_fields, ARRAY_SIZE(thing_fields)};

static char seen_key[WEB_API_SCOPED_KEY_MAX_LEN + 1];
static uint32_t seen_hash;
static char seen_param[WEB_API_PARAM_MAX_LEN + 1];
static bool handler_ran;

static void h_ok(struct web_api_call *call)
{
	struct web_json_writer *w = web_api_json(call);

	handler_ran = true;
	memcpy(seen_key, call->scoped_key, sizeof(seen_key));
	seen_hash = call->request_hash;
	memcpy(seen_param, call->params[0], sizeof(seen_param));
	web_json_object_begin(w);
	web_json_key(w, "ok");
	web_json_bool(w, true);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

static void h_silent(struct web_api_call *call)
{
	ARG_UNUSED(call);
	handler_ran = true;
}

static void h_huge(struct web_api_call *call)
{
	static char big[CONFIG_WEB_API_RESPONSE_BODY_MAX + 8];
	struct web_json_writer *w = web_api_json(call);

	memset(big, 'x', sizeof(big) - 1);
	web_json_string(w, big);
	web_api_reply_json(call, 200);
}

static void h_empty(struct web_api_call *call)
{
	web_api_reply_empty(call, 204);
}

static const char *const thing_query[] = {"limit", "cursor", NULL};

static const struct web_api_route test_routes[] = {
	{"openThing", WEB_API_GET, "/open", WEB_API_PUBLIC, NULL, 0, NULL, h_ok},
	{"getThing", WEB_API_GET, "/things/{thing_id}", 0, NULL, 0, thing_query, h_ok},
	{"putThing", WEB_API_PUT, "/things/{thing_id}",
	 WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, &thing_schema,
	 sizeof(struct thing_body), NULL, h_ok},
	{"postThing", WEB_API_POST, "/things", WEB_API_CSRF | WEB_API_IDEMPOTENT, &thing_schema,
	 sizeof(struct thing_body), NULL, h_ok},
	{"deleteThing", WEB_API_DELETE, "/things/{thing_id}", WEB_API_CSRF, NULL, 0, NULL, h_empty},
	{"silent", WEB_API_GET, "/silent", WEB_API_PUBLIC, NULL, 0, NULL, h_silent},
	{"huge", WEB_API_GET, "/huge", WEB_API_PUBLIC, NULL, 0, NULL, h_huge},
	{"openSetup", WEB_API_POST, "/setup", WEB_API_PUBLIC | WEB_API_ORIGIN | WEB_API_SETUP_TOKEN,
	 NULL, 0, NULL, h_ok},
};

static const struct web_api_router test_router = {test_routes, ARRAY_SIZE(test_routes)};

static struct web_auth_session session;
static char cookie[96];

static void signed_in(void)
{
	struct web_auth_state s;

	web_auth_get_state(&s);
	zassert_equal(web_auth_setup(s.setup_token, "a good long password", 20, NULL, &session)
			      .status,
		      WEB_AUTH_OK);
	snprintf(cookie, sizeof(cookie), "theme=dark; cedar_session=%s", session.token);
}

static void authorised(enum web_api_method m, const char *path)
{
	request(m, path);
	ctx.req.headers.cookie = cookie;
	ctx.req.headers.csrf_token = session.csrf_token;
	ctx.req.headers.idempotency_key = "key-0123456789abcdef";
}

static void expect_error(uint16_t status, const char *code)
{
	char fragment[64];

	zassert_equal(ctx.rsp.status, status, "status %u, body %s", ctx.rsp.status, body);
	snprintf(fragment, sizeof(fragment), "\"code\":\"%s\"", code);
	zassert_true(body_has(fragment), "expected %s in %s", code, body);
	zassert_false(handler_ran, "the handler must not run for a rejection");
}

static void before(void *f)
{
	ARG_UNUSED(f);
	harness_reset();
	handler_ran = false;
	signed_in();
}

ZTEST_SUITE(middleware, NULL, NULL, before, NULL, NULL);

/* -- helpers ------------------------------------------------------------- */

ZTEST(middleware, test_host_allowed)
{
	static const char *const allowed[] = {
		"192.168.88.14", "192.168.88.14:80", "[fe80::8234:28ff:fe10:1273]",
		"[fe80::1]:8080", "[::1]", "localhost", "LocalHost:3000", "cedar.lan",
		"switch.LOCAL", "switch.local:80",
	};
	static const char *const refused[] = {
		"", "evil.example", "cedar.lan.evil", "192.168.88.14:99999", "192.168.88.14:",
		"[fe80::1", "fe80::1", "010.0.0.1", "192.168.88", "[fe80::1]x", "localhost.",
		"192.168.88.14 ",
	};

	zassert_true(web_api_host_allowed(NULL), "no Host is not a browser");
	for (size_t i = 0; i < ARRAY_SIZE(allowed); i++) {
		zassert_true(web_api_host_allowed(allowed[i]), "%s", allowed[i]);
	}
	for (size_t i = 0; i < ARRAY_SIZE(refused); i++) {
		zassert_false(web_api_host_allowed(refused[i]), "'%s'", refused[i]);
	}
}

ZTEST(middleware, test_cookie_session)
{
	char out[WEB_AUTH_TOKEN_LEN + 1];

	zassert_true(web_api_cookie_session("cedar_session=abc", out, sizeof(out)));
	zassert_str_equal(out, "abc");
	zassert_true(web_api_cookie_session("a=1;  cedar_session=xyz ; b=2", out, sizeof(out)));
	zassert_str_equal(out, "xyz");
	zassert_false(web_api_cookie_session(NULL, out, sizeof(out)));
	zassert_false(web_api_cookie_session("a=1; b=2", out, sizeof(out)));
	zassert_false(web_api_cookie_session("cedar_sessionX=abc", out, sizeof(out)));
	zassert_false(web_api_cookie_session("xcedar_session=abc", out, sizeof(out)));
	zassert_false(web_api_cookie_session("cedar_session=", out, sizeof(out)));
	zassert_false(web_api_cookie_session("cedar_session=a; cedar_session=b", out, sizeof(out)),
		      "two values: which one was meant cannot be known");
	zassert_false(web_api_cookie_session("cedar_session=0123456789012345678901234567890123456789"
					     "0123456789",
					     out, sizeof(out)),
		      "too long for a token");
}

ZTEST(middleware, test_method_names)
{
	zassert_equal(web_api_method_from_str("GET"), WEB_API_GET);
	zassert_equal(web_api_method_from_str("DELETE"), WEB_API_DELETE);
	zassert_equal(web_api_method_from_str("get"), WEB_API_METHOD_OTHER, "methods are case-sensitive");
	zassert_equal(web_api_method_from_str("PATCH"), WEB_API_METHOD_OTHER);
	zassert_equal(web_api_method_from_str(NULL), WEB_API_METHOD_OTHER);
}

/* -- 1. Host --------------------------------------------------------------- */

ZTEST(middleware, test_host_is_checked_before_routing)
{
	request(WEB_API_GET, "/api/v1/nowhere");
	ctx.req.headers.host = "attacker.example";
	dispatch(&test_router);
	expect_error(403, "origin_rejected");
}

/* -- 2. dropped headers ---------------------------------------------------- */

ZTEST(middleware, test_dropped_headers_are_refused_before_the_session)
{
	authorised(WEB_API_GET, "/api/v1/things/t1");
	ctx.req.headers.dropped = true;
	dispatch(&test_router);
	expect_error(422, "validation_failed");
	zassert_false(body_has("\"fields\""), "no pointer syntax for headers");
}

/* -- 3. routing ------------------------------------------------------------ */

ZTEST(middleware, test_unknown_paths_are_json_404)
{
	static const char *const paths[] = {
		"/api/v1/nowhere", "/api/v2/open", "/api/v1/open/", "/api/v1", "/api/v1/",
		"/api/v1/things/t1/extra", "/api/v1//open", "/api/open",
	};

	for (size_t i = 0; i < ARRAY_SIZE(paths); i++) {
		request(WEB_API_GET, paths[i]);
		dispatch(&test_router);
		expect_error(404, "not_found");
	}
}

ZTEST(middleware, test_undeclared_method_is_404)
{
	request(WEB_API_POST, "/api/v1/open");
	dispatch(&test_router);
	expect_error(404, "not_found");
	zassert_true(body_has("method"), "the message says what happened");

	request(WEB_API_METHOD_OTHER, "/api/v1/open");
	dispatch(&test_router);
	expect_error(404, "not_found");
}

ZTEST(middleware, test_path_parameters_must_be_opaque_ids)
{
	char long_id[96];

	authorised(WEB_API_GET, "/api/v1/things/bad!id");
	dispatch(&test_router);
	expect_error(404, "not_found");

	/* 65 characters: one past the opaque id limit. */
	strcpy(long_id, "/api/v1/things/");
	memset(long_id + 15, 'a', 65);
	long_id[15 + 65] = '\0';
	authorised(WEB_API_GET, long_id);
	dispatch(&test_router);
	expect_error(404, "not_found");

	authorised(WEB_API_GET, "/api/v1/things/Thing_42-x");
	dispatch(&test_router);
	zassert_equal(ctx.rsp.status, 200);
	zassert_str_equal(seen_param, "Thing_42-x");
}

ZTEST(middleware, test_web_api_not_found)
{
	web_api_not_found(&ctx);
	zassert_equal(ctx.rsp.status, 404);
	zassert_not_null(ctx.rsp.body);
}

/* -- 4. session ----------------------------------------------------------- */

ZTEST(middleware, test_session_required)
{
	request(WEB_API_GET, "/api/v1/things/t1");
	dispatch(&test_router);
	expect_error(401, "authentication_required");

	request(WEB_API_GET, "/api/v1/things/t1");
	ctx.req.headers.cookie = "cedar_session=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
	dispatch(&test_router);
	expect_error(401, "authentication_required");

	zassert_equal(web_auth_logout(session.token), WEB_AUTH_OK);
	authorised(WEB_API_GET, "/api/v1/things/t1");
	dispatch(&test_router);
	expect_error(401, "session_expired");
	zassert_true(body_has("\"retryable\":true"), "signing in again fixes it");
}

ZTEST(middleware, test_public_route_needs_no_session)
{
	request(WEB_API_GET, "/api/v1/open");
	dispatch(&test_router);
	zassert_equal(ctx.rsp.status, 200);
}

/* -- 5. origin ------------------------------------------------------------ */

ZTEST(middleware, test_origin)
{
	static const struct {
		const char *origin;
		bool ok;
	} cases[] = {
		{NULL, true},
		{"http://192.168.88.14", true},
		{"https://192.168.88.14", true},
		{"http://192.168.88.14:8080", false},
		{"http://evil.example", false},
		{"null", false},
		{"192.168.88.14", false},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		handler_ran = false;
		request(WEB_API_POST, "/api/v1/setup");
		ctx.req.headers.origin = cases[i].origin;
		ctx.req.headers.setup_token = "0123456789abcdef";
		dispatch(&test_router);
		if (cases[i].ok) {
			zassert_equal(ctx.rsp.status, 200, "%s", cases[i].origin);
		} else {
			expect_error(403, "origin_rejected");
		}
	}
}

ZTEST(middleware, test_origin_before_setup_token)
{
	request(WEB_API_POST, "/api/v1/setup");
	ctx.req.headers.origin = "http://evil.example";
	dispatch(&test_router);
	expect_error(403, "origin_rejected");
}

/* -- 6. CSRF --------------------------------------------------------------- */

ZTEST(middleware, test_authentication_comes_before_csrf)
{
	request(WEB_API_DELETE, "/api/v1/things/t1");
	dispatch(&test_router);
	expect_error(401, "authentication_required");
}

ZTEST(middleware, test_csrf)
{
	authorised(WEB_API_DELETE, "/api/v1/things/t1");
	ctx.req.headers.csrf_token = NULL;
	dispatch(&test_router);
	expect_error(403, "csrf_failed");

	authorised(WEB_API_DELETE, "/api/v1/things/t1");
	ctx.req.headers.csrf_token = session.token;
	dispatch(&test_router);
	expect_error(403, "csrf_failed");

	authorised(WEB_API_DELETE, "/api/v1/things/t1");
	dispatch(&test_router);
	zassert_equal(ctx.rsp.status, 204);
}

ZTEST(middleware, test_csrf_comes_before_the_body)
{
	authorised(WEB_API_PUT, "/api/v1/things/t1");
	ctx.req.headers.csrf_token = "wrong";
	request_body("{not json");
	dispatch(&test_router);
	expect_error(403, "csrf_failed");
}

/* -- 7. headers ------------------------------------------------------------ */

ZTEST(middleware, test_setup_token_header)
{
	request(WEB_API_POST, "/api/v1/setup");
	dispatch(&test_router);
	expect_error(403, "setup_not_allowed");

	request(WEB_API_POST, "/api/v1/setup");
	ctx.req.headers.setup_token = "short";
	dispatch(&test_router);
	expect_error(422, "validation_failed");
}

ZTEST(middleware, test_idempotency_key_header)
{
	authorised(WEB_API_PUT, "/api/v1/things/t1");
	ctx.req.headers.idempotency_key = NULL;
	request_body("{not json");
	dispatch(&test_router);
	expect_error(422, "validation_failed");
	zassert_true(body_has("The Idempotency-Key header is required"),
		     "absent is told apart from malformed: %s", body);

	authorised(WEB_API_PUT, "/api/v1/things/t1");
	ctx.req.headers.idempotency_key = "too-short";
	request_body("{\"name\":\"a\"}");
	dispatch(&test_router);
	expect_error(422, "validation_failed");

	authorised(WEB_API_PUT, "/api/v1/things/t1");
	ctx.req.headers.idempotency_key = "has spaces in it, sixteen+";
	request_body("{\"name\":\"a\"}");
	dispatch(&test_router);
	expect_error(422, "validation_failed");
}

/* -- 8. body --------------------------------------------------------------- */

ZTEST(middleware, test_body_steps_in_order)
{
	authorised(WEB_API_PUT, "/api/v1/things/t1");
	dispatch(&test_router);
	expect_error(400, "invalid_json");

	authorised(WEB_API_PUT, "/api/v1/things/t1");
	request_body("{\"name\":\"a\"}");
	ctx.req.headers.content_type = "text/plain";
	dispatch(&test_router);
	expect_error(415, "unsupported_media_type");

	authorised(WEB_API_PUT, "/api/v1/things/t1");
	ctx.req.body_received = CONFIG_WEB_API_JSON_BODY_MAX + 1;
	ctx.req.headers.content_type = "application/json";
	dispatch(&test_router);
	expect_error(413, "payload_too_large");

	authorised(WEB_API_PUT, "/api/v1/things/t1");
	request_body("{\"name\":");
	dispatch(&test_router);
	expect_error(400, "invalid_json");

	authorised(WEB_API_PUT, "/api/v1/things/t1");
	request_body("{\"name\":\"\",\"extra\":1,\"count\":11}");
	dispatch(&test_router);
	expect_error(422, "validation_failed");
	zassert_true(body_has("{\"path\":\"/count\",\"code\":\"out_of_range\"}"), "%s", body);
	zassert_true(body_has("{\"path\":\"/extra\",\"code\":\"unknown_field\"}"), "%s", body);
	zassert_true(body_has("{\"path\":\"/name\",\"code\":\"out_of_range\"}"), "%s", body);
}

ZTEST(middleware, test_content_type_parameters_and_case)
{
	authorised(WEB_API_PUT, "/api/v1/things/t1");
	request_body("{\"name\":\"a\"}");
	ctx.req.headers.content_type = "Application/JSON; charset=utf-8";
	dispatch(&test_router);
	zassert_equal(ctx.rsp.status, 200, "%s", body);
}

ZTEST(middleware, test_body_without_a_schema_is_415)
{
	authorised(WEB_API_DELETE, "/api/v1/things/t1");
	request_body("{}");
	dispatch(&test_router);
	expect_error(415, "unsupported_media_type");
}

ZTEST(middleware, test_optional_body_may_be_absent)
{
	authorised(WEB_API_POST, "/api/v1/things");
	dispatch(&test_router);
	zassert_equal(ctx.rsp.status, 200);
}

/* -- 9. query -------------------------------------------------------------- */

ZTEST(middleware, test_query)
{
	static const struct {
		const char *path;
		bool ok;
	} cases[] = {
		{"/api/v1/things/t1?limit=5", true},
		{"/api/v1/things/t1?limit=5&cursor=abc", true},
		{"/api/v1/things/t1?", true},
		{"/api/v1/things/t1?&&", true},
		{"/api/v1/things/t1?%6Cimit=5", true},
		{"/api/v1/things/t1?limit=5&limit=6", false},
		{"/api/v1/things/t1?other=1", false},
		{"/api/v1/things/t1?limit", true},
		{"/api/v1/open?x=1", false},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		handler_ran = false;
		authorised(WEB_API_GET, cases[i].path);
		dispatch(&test_router);
		if (cases[i].ok) {
			zassert_equal(ctx.rsp.status, 200, "%s: %s", cases[i].path, body);
		} else {
			expect_error(400, "invalid_query");
		}
	}
}

/* -- 10. handler and idempotency scope ------------------------------------ */

static void put_thing(const char *path, const char *json, const char *key)
{
	authorised(WEB_API_PUT, path);
	ctx.req.headers.idempotency_key = key;
	request_body(json);
	dispatch(&test_router);
	zassert_equal(ctx.rsp.status, 200, "%s", body);
}

ZTEST(middleware, test_scoped_key_names_the_operation)
{
	char a[sizeof(seen_key)], b[sizeof(seen_key)];
	const char *key = "same-client-key-0001";

	put_thing("/api/v1/things/t1", "{\"name\":\"a\"}", key);
	memcpy(a, seen_key, sizeof(a));
	zassert_true(strlen(a) == 16 + 1 + strlen(key));
	zassert_equal(a[16], ':');
	zassert_str_equal(a + 17, key);

	put_thing("/api/v1/things/t2", "{\"name\":\"a\"}", key);
	memcpy(b, seen_key, sizeof(b));
	zassert_not_equal(strcmp(a, b), 0, "another URL is another action");

	authorised(WEB_API_POST, "/api/v1/things");
	ctx.req.headers.idempotency_key = key;
	request_body("{\"name\":\"a\"}");
	dispatch(&test_router);
	zassert_not_equal(strncmp(a, seen_key, 16), 0, "another method is another action");

	put_thing("/api/v1/things/t1", "{\"name\":\"a\"}", "another-client-key-2");
	zassert_equal(strncmp(a, seen_key, 16), 0, "the scope does not depend on the key");
}

ZTEST(middleware, test_request_hash_is_over_content_not_spelling)
{
	uint32_t first;

	put_thing("/api/v1/things/t1", "{\"name\":\"a\",\"count\":3}", "key-0123456789abcdef");
	first = seen_hash;
	put_thing("/api/v1/things/t1", " { \"count\" : 3.0 , \"name\" : \"\\u0061\" } ",
		  "key-0123456789abcdef");
	zassert_equal(seen_hash, first, "whitespace, order and spelling are not content");
	put_thing("/api/v1/things/t1", "{\"name\":\"a\",\"count\":4}", "key-0123456789abcdef");
	zassert_not_equal(seen_hash, first);
}

ZTEST(middleware, test_handler_without_reply_is_500)
{
	request(WEB_API_GET, "/api/v1/silent");
	dispatch(&test_router);
	zassert_equal(ctx.rsp.status, 500);
	zassert_true(body_has("\"code\":\"internal_error\""));
}

ZTEST(middleware, test_oversized_response_is_500_not_truncated)
{
	request(WEB_API_GET, "/api/v1/huge");
	dispatch(&test_router);
	zassert_equal(ctx.rsp.status, 500);
	zassert_true(body_has("\"code\":\"internal_error\""));
}

ZTEST(middleware, test_no_content_closes_the_connection)
{
	authorised(WEB_API_DELETE, "/api/v1/things/t1");
	dispatch(&test_router);
	zassert_equal(ctx.rsp.status, 204);
	zassert_is_null(ctx.rsp.body);
	zassert_true(ctx.rsp.close_connection);

	authorised(WEB_API_GET, "/api/v1/things/t1");
	dispatch(&test_router);
	zassert_false(ctx.rsp.close_connection);
}

ZTEST(middleware, test_request_ids_differ)
{
	char first[API_REQUEST_ID_MAX_LEN + 1];

	request(WEB_API_GET, "/api/v1/open");
	dispatch(&test_router);
	strcpy(first, response_header("X-Request-ID"));
	request(WEB_API_GET, "/api/v1/open");
	dispatch(&test_router);
	zassert_not_equal(strcmp(first, response_header("X-Request-ID")), 0);
}

ZTEST(middleware, test_body_limit_lookup)
{
	zassert_equal(web_api_body_limit(&test_router, WEB_API_PUT, "/api/v1/things/x"),
		      CONFIG_WEB_API_JSON_BODY_MAX);
	zassert_equal(web_api_body_limit(&test_router, WEB_API_DELETE, "/api/v1/things/x"), 0);
	zassert_equal(web_api_body_limit(&test_router, WEB_API_PUT, "/api/v1/nowhere"), 0);
}

/* -- query values (web_api_query_get) ----------------------------------- */

static int query_value(const char *query, const char *name, char *out, size_t cap)
{
	const struct web_api_request req = {.query = query};

	return web_api_query_get(&req, name, out, cap);
}

ZTEST(middleware, test_query_get_decodes_form_encoding)
{
	char v[16];

	zassert_equal(query_value("a=1&contains=x+y%2Az", "contains", v, sizeof(v)), 5);
	zassert_str_equal(v, "x y*z");
	zassert_equal(query_value("contains=%41%62", "contains", v, sizeof(v)), 2);
	zassert_str_equal(v, "Ab", "two hex digits are one byte");
	zassert_equal(query_value("contains=100%", "contains", v, sizeof(v)), 4);
	zassert_str_equal(v, "100%", "a percent sign without two hex digits is itself");
	zassert_equal(query_value("contains=%4", "contains", v, sizeof(v)), 2);
	zassert_str_equal(v, "%4");
	zassert_equal(query_value("contains=%zz", "contains", v, sizeof(v)), 3);
	zassert_str_equal(v, "%zz");
}

ZTEST(middleware, test_query_get_finds_the_named_parameter)
{
	char v[16];

	zassert_equal(query_value("limit=5", "lim", v, sizeof(v)), -ENOENT,
		      "a name is not a prefix of another");
	zassert_equal(query_value("lim=1&limit=5", "limit", v, sizeof(v)), 1);
	zassert_str_equal(v, "5");
	zassert_equal(query_value("a=1&b=2&c=3", "c", v, sizeof(v)), 1);
	zassert_str_equal(v, "3", "the last parameter is found");
	zassert_equal(query_value("a=1&b=2", "c", v, sizeof(v)), -ENOENT);
	zassert_equal(query_value("", "a", v, sizeof(v)), -ENOENT);
	zassert_equal(query_value("cursor=", "cursor", v, sizeof(v)), 0);
	zassert_str_equal(v, "", "present and empty");
	zassert_equal(query_value("cursor", "cursor", v, sizeof(v)), 0, "a name without '='");
}

ZTEST(middleware, test_query_get_bounds)
{
	char v[4];

	zassert_equal(query_value("a=abc", "a", v, sizeof(v)), 3, "exactly fits with its NUL");
	zassert_str_equal(v, "abc");
	zassert_equal(query_value("a=abcd", "a", v, sizeof(v)), -ENOSPC);
	zassert_equal(query_value("a=%00", "a", v, sizeof(v)), -EINVAL, "no NUL inside a C string");
	zassert_equal(query_value("a=x", "a", v, 0), -ENOSPC);
}
