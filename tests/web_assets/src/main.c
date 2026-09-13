/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * web-assets policy on a hand-built table. The same cases as
 * tools/api-contract/tests/test_mock_static.py, which transcribes this module
 * for the mock, so the two cannot drift apart unnoticed.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <web_assets/web_assets.h>

static const uint8_t page[] = "<!doctype html>";
static const uint8_t script[] = {0x1f, 0x8b, 0x08, 0x00};
static const uint8_t icon[] = "<svg/>";

/* Sorted by path, as the generator emits it. */
static const struct web_asset assets[] = {
	{"/assets/app-AbCdEf12.js", "text/javascript; charset=utf-8", "\"1111111111111111\"", script,
	 sizeof(script), true, WEB_ASSET_CACHE_IMMUTABLE, false},
	{"/favicon.svg", "image/svg+xml", "\"2222222222222222\"", icon, sizeof(icon) - 1, false,
	 WEB_ASSET_CACHE_REVALIDATE, false},
	{"/index.html", "text/html; charset=utf-8", "\"3333333333333333\"", page, sizeof(page) - 1,
	 false, WEB_ASSET_CACHE_REVALIDATE, true},
};

static const struct web_assets_table table = {assets, ARRAY_SIZE(assets), &assets[2], "t-1"};

/* A page that is itself gzipped: the most headers any answer carries. */
static const struct web_asset gz_assets[] = {
	{"/index.html", "text/html; charset=utf-8", "\"4444444444444444\"", script, sizeof(script),
	 true, WEB_ASSET_CACHE_REVALIDATE, true},
};
static const struct web_assets_table gz_table = {gz_assets, 1, &gz_assets[0], "t-2"};

static struct web_assets_response rsp;

static void get(const struct web_assets_table *t, const char *path, const char *ae,
		const char *inm)
{
	const struct web_assets_request req = {
		.is_get = true, .path = path, .accept_encoding = ae, .if_none_match = inm};

	web_assets_respond(t, &req, &rsp);
}

static const char *header(const char *name)
{
	for (size_t i = 0; i < rsp.header_count; i++) {
		if (strcmp(rsp.headers[i].name, name) == 0) {
			return rsp.headers[i].value;
		}
	}
	return NULL;
}

ZTEST_SUITE(web_assets, NULL, NULL, NULL, NULL, NULL);

ZTEST(web_assets, test_find)
{
	zassert_equal(web_assets_find(&table, "/assets/app-AbCdEf12.js"), &assets[0]);
	zassert_equal(web_assets_find(&table, "/favicon.svg"), &assets[1]);
	zassert_equal(web_assets_find(&table, "/index.html"), &assets[2]);
	zassert_is_null(web_assets_find(&table, "/"));
	zassert_is_null(web_assets_find(&table, "/a"));
	zassert_is_null(web_assets_find(&table, "/zzz"));
	zassert_is_null(web_assets_find(&table, "/index.htm"));
}

ZTEST(web_assets, test_accepts_gzip)
{
	static const struct {
		const char *value;
		bool accepted;
	} cases[] = {
		{NULL, false},          {"gzip", true},         {"GZIP", true},
		{"x-gzip", true},       {"deflate, br", false}, {"*", true},
		{"gzip;q=0", false},    {"gzip; q=0.000", false}, {"gzip;q=0.5", true},
		{"*;q=0, gzip", true},  {"gzip;q=0, *", false}, {"br, *;q=0", false},
		{" gzip , br", true},   {"", false},            {"gzipx", false},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		zassert_equal(web_assets_accepts_gzip(cases[i].value), cases[i].accepted, "'%s'",
			      cases[i].value ? cases[i].value : "(null)");
	}
}

ZTEST(web_assets, test_etag_matches)
{
	static const struct {
		const char *value;
		bool match;
	} cases[] = {
		{NULL, false},        {"\"abc\"", true},  {"W/\"abc\"", true},
		{"\"x\", \"abc\"", true}, {"*", true},    {"\"abd\"", false},
		{"abc", false},       {" \"abc\" ", true}, {"\"abc\"x", false},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		zassert_equal(web_assets_etag_matches(cases[i].value, "\"abc\""), cases[i].match, "'%s'",
			      cases[i].value ? cases[i].value : "(null)");
	}
	zassert_true(web_assets_etag_matches("\"abc\"", "W/\"abc\""), "weak on the stored side too");
}

ZTEST(web_assets, test_page)
{
	get(&table, "/", NULL, NULL);
	zassert_equal(rsp.status, 200);
	zassert_equal(rsp.body, page);
	zassert_equal(rsp.body_len, sizeof(page) - 1);
	zassert_str_equal(header("Content-Type"), "text/html; charset=utf-8");
	zassert_str_equal(header("ETag"), "\"3333333333333333\"");
	zassert_str_equal(header("Cache-Control"), "no-cache");
	zassert_str_equal(header("X-Content-Type-Options"), "nosniff");
	zassert_str_equal(header("Content-Security-Policy"), web_assets_page_csp);
	zassert_str_equal(header("Referrer-Policy"), "no-referrer");
	zassert_str_equal(header("X-Frame-Options"), "DENY");
	zassert_is_null(header("Content-Encoding"));
	zassert_is_null(header("Vary"));
	zassert_false(rsp.close_connection);
	zassert_true(strstr(web_assets_page_csp, "default-src 'self'") == web_assets_page_csp);
	zassert_not_null(strstr(web_assets_page_csp, "frame-ancestors 'none'"));
}

ZTEST(web_assets, test_navigation_fallback)
{
	static const char *const navigations[] = {"/access", "/network/wifi", "/a/b/c"};
	static const char *const not_found[] = {"/assets/missing", "/assets/x-AbCdEf12.js",
						"/robots.txt", "/a/b.c"};

	for (size_t i = 0; i < ARRAY_SIZE(navigations); i++) {
		get(&table, navigations[i], NULL, NULL);
		zassert_equal(rsp.status, 200, "%s", navigations[i]);
		zassert_equal(rsp.body, page, "%s gets the page", navigations[i]);
	}
	for (size_t i = 0; i < ARRAY_SIZE(not_found); i++) {
		get(&table, not_found[i], NULL, NULL);
		zassert_equal(rsp.status, 404, "%s", not_found[i]);
		zassert_str_equal(header("Content-Type"), "text/plain; charset=utf-8");
		zassert_str_equal(header("Cache-Control"), "no-store");
	}
}

ZTEST(web_assets, test_only_get)
{
	const struct web_assets_request req = {.is_get = false, .path = "/"};

	web_assets_respond(&table, &req, &rsp);
	zassert_equal(rsp.status, 405);
}

ZTEST(web_assets, test_gzip_negotiation)
{
	get(&table, "/assets/app-AbCdEf12.js", NULL, NULL);
	zassert_equal(rsp.status, 406);
	zassert_str_equal(header("Vary"), "Accept-Encoding");

	get(&table, "/assets/app-AbCdEf12.js", "gzip;q=0", NULL);
	zassert_equal(rsp.status, 406);

	get(&table, "/assets/app-AbCdEf12.js", "br, gzip", NULL);
	zassert_equal(rsp.status, 200);
	zassert_equal(rsp.body, script);
	zassert_str_equal(header("Content-Encoding"), "gzip");
	zassert_str_equal(header("Vary"), "Accept-Encoding");
	zassert_str_equal(header("Cache-Control"), "public, max-age=31536000, immutable");
	zassert_is_null(header("Content-Security-Policy"), "only the page carries the policy");

	get(&table, "/favicon.svg", NULL, NULL);
	zassert_equal(rsp.status, 200, "identity files need no negotiation");
	zassert_is_null(header("Vary"));
}

ZTEST(web_assets, test_revalidation)
{
	get(&table, "/assets/app-AbCdEf12.js", "gzip", "W/\"1111111111111111\"");
	zassert_equal(rsp.status, 304);
	zassert_is_null(rsp.body);
	zassert_equal(rsp.body_len, 0);
	zassert_true(rsp.close_connection, "the stray chunk terminator must not be read");
	zassert_str_equal(header("ETag"), "\"1111111111111111\"");
	zassert_str_equal(header("Cache-Control"), "public, max-age=31536000, immutable");
	zassert_str_equal(header("Vary"), "Accept-Encoding");
	zassert_is_null(header("Content-Type"));

	get(&table, "/", NULL, "\"3333333333333333\"");
	zassert_equal(rsp.status, 304);

	get(&table, "/", NULL, "\"0000000000000000\"");
	zassert_equal(rsp.status, 200);

	/* Negotiation first: a matching validator does not turn a 406 into a 304. */
	get(&table, "/assets/app-AbCdEf12.js", NULL, "\"1111111111111111\"");
	zassert_equal(rsp.status, 406);
}

ZTEST(web_assets, test_every_header_of_the_largest_answer_fits)
{
	get(&gz_table, "/", "gzip", NULL);
	zassert_equal(rsp.status, 200);
	zassert_equal(rsp.header_count, 9);
	zassert_true(rsp.header_count <= WEB_ASSETS_MAX_HEADERS);
	zassert_not_null(header("X-Frame-Options"), "the last one was not dropped");
}
