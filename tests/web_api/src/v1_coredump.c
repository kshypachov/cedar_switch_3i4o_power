/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The coredump bindings (src/web/api/v1/system.c: getCoredump, downloadCoredump,
 * clearCoredump) through the real route table, over fake hooks: native_sim has
 * no Zephyr coredump, and the board's hooks (src/diagnostic/coredump_support.c)
 * only forward to it. Part of the v1 suite, whose before() resets web-auth;
 * each test here installs the hooks it needs.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "harness.h"
#include "web_api_v1.h"

#define PASSWORD   "correct horse battery"
#define LAN_ORIGIN "http://192.168.88.14"

static char session_cookie[96];
static char csrf[WEB_AUTH_TOKEN_LEN + 1];

/* -- the fake stored dump ------------------------------------------------------------ */

/* Larger than the 16 KiB response buffer, so the download takes several pieces. */
static uint8_t dump[40000];
static int dump_len;
static int size_error;
static int read_error_at = -1;
static int clears;

static int fake_size(void)
{
	return size_error != 0 ? size_error : dump_len;
}

static int fake_read(size_t offset, uint8_t *buf, size_t len)
{
	if (read_error_at >= 0 && offset >= (size_t)read_error_at) {
		return -EIO;
	}
	if (offset >= (size_t)dump_len) {
		return 0;
	}
	len = MIN(len, (size_t)dump_len - offset);
	memcpy(buf, &dump[offset], len);
	return (int)len;
}

static int fake_clear(void)
{
	clears++;
	dump_len = 0;
	return 0;
}

static const struct web_api_v1_coredump hooks = {
	.size = fake_size,
	.read = fake_read,
	.clear = fake_clear,
};

/* A Zephyr coredump header ("ZE", version 2, Cortex-M, 32-bit, reason) and filler. */
static void store_dump(size_t len, uint32_t reason)
{
	for (size_t i = 0; i < len; i++) {
		dump[i] = (uint8_t)(i * 7U + 3U);
	}
	dump[0] = 'Z';
	dump[1] = 'E';
	sys_put_le16(2, &dump[2]);
	sys_put_le16(3, &dump[4]);
	dump[6] = 5;
	dump[7] = 0;
	sys_put_le32(reason, &dump[8]);
	dump_len = (int)len;
}

static void install(void)
{
	dump_len = 0;
	size_error = 0;
	read_error_at = -1;
	clears = 0;
	web_api_v1_set_coredump(&hooks);
}

/* -- requests -------------------------------------------------------------------------- */

static bool json_string(const char *text, const char *name, char *out, size_t cap)
{
	char key[48];
	const char *p;
	const char *end;

	snprintf(key, sizeof(key), "\"%s\":\"", name);
	p = strstr(text, key);
	if (p == NULL) {
		return false;
	}
	p += strlen(key);
	end = strchr(p, '"');
	if (end == NULL || (size_t)(end - p) >= cap) {
		return false;
	}
	memcpy(out, p, end - p);
	out[end - p] = '\0';
	return true;
}

static void sign_in(void)
{
	char token[WEB_AUTH_SETUP_TOKEN_LEN + 1];
	const char *set_cookie;
	const char *semi;

	request(WEB_API_GET, "/api/v1/auth/state");
	dispatch(&web_api_v1_router);
	zassert_true(json_string(body, "setup_token", token, sizeof(token)));
	request(WEB_API_POST, "/api/v1/auth/setup");
	ctx.req.headers.origin = LAN_ORIGIN;
	ctx.req.headers.setup_token = token;
	request_body("{\"password\":\"" PASSWORD "\"}");
	dispatch(&web_api_v1_router);
	zassert_equal(ctx.rsp.status, 201, "%s", body);
	zassert_true(json_string(body, "csrf_token", csrf, sizeof(csrf)));
	set_cookie = response_header("Set-Cookie");
	semi = strchr(set_cookie, ';');
	memcpy(session_cookie, set_cookie, semi - set_cookie);
	session_cookie[semi - set_cookie] = '\0';
}

static void get(const char *path)
{
	request(WEB_API_GET, path);
	ctx.req.headers.cookie = session_cookie;
	dispatch(&web_api_v1_router);
}

static void clear(bool with_csrf)
{
	request(WEB_API_DELETE, "/api/v1/system/coredump");
	ctx.req.headers.cookie = session_cookie;
	ctx.req.headers.origin = LAN_ORIGIN;
	if (with_csrf) {
		ctx.req.headers.csrf_token = csrf;
	}
	dispatch(&web_api_v1_router);
}

static void expect_error(uint16_t status, const char *code)
{
	char fragment[64];

	snprintf(fragment, sizeof(fragment), "\"code\":\"%s\"", code);
	zassert_equal(ctx.rsp.status, status, "%s", body);
	zassert_true(body_has(fragment), "%s", body);
}

static uint8_t downloaded[sizeof(dump)];

/* Pull every piece of the stream the last dispatch started; the bytes, or -1 on an error. */
static int pull_all(int *pieces)
{
	size_t n = 0;
	bool final = false;

	*pieces = 0;
	for (int guard = 0; !final && guard < 100; guard++) {
		if (web_api_stream_pull(&ctx, &final) != 0) {
			return -1;
		}
		zassert_true(n + ctx.rsp.body_len <= sizeof(downloaded));
		memcpy(&downloaded[n], ctx.rsp.body, ctx.rsp.body_len);
		n += ctx.rsp.body_len;
		*pieces += ctx.rsp.body_len > 0 ? 1 : 0;
	}
	zassert_true(final);
	zassert_false(web_api_stream_active(&ctx));
	return (int)n;
}

/* -- tests ------------------------------------------------------------------------------- */

ZTEST(v1, test_coredump_without_hooks_is_unavailable)
{
	sign_in();
	web_api_v1_set_coredump(NULL);

	get("/api/v1/system/coredump");
	expect_error(503, "capability_unavailable");
	get("/api/v1/system/coredump/data");
	expect_error(503, "capability_unavailable");
	clear(true);
	expect_error(503, "capability_unavailable");
}

ZTEST(v1, test_coredump_needs_a_session)
{
	install();
	request(WEB_API_GET, "/api/v1/system/coredump");
	dispatch(&web_api_v1_router);
	expect_error(401, "authentication_required");
	request(WEB_API_GET, "/api/v1/system/coredump/data");
	dispatch(&web_api_v1_router);
	expect_error(401, "authentication_required");
}

ZTEST(v1, test_coredump_none_stored)
{
	sign_in();
	install();

	get("/api/v1/system/coredump");
	zassert_equal(ctx.rsp.status, 200, "%s", body);
	zassert_str_equal(body, "{\"coredump\":null}");
	get("/api/v1/system/coredump/data");
	expect_error(404, "not_found");
}

ZTEST(v1, test_coredump_describes_the_stored_dump)
{
	sign_in();
	install();
	store_dump(30000, 4);

	get("/api/v1/system/coredump");
	zassert_equal(ctx.rsp.status, 200, "%s", body);
	zassert_str_equal(body, "{\"coredump\":{\"size_bytes\":30000,\"reason\":\"kernel_panic\","
				"\"reason_code\":4}}");

	/* Architecture codes are CPU exceptions. */
	store_dump(100, 17);
	get("/api/v1/system/coredump");
	zassert_true(body_has("\"reason\":\"cpu_exception\",\"reason_code\":17"), "%s", body);
}

ZTEST(v1, test_coredump_without_its_header_has_no_reason)
{
	sign_in();
	install();
	store_dump(100, 0);
	dump[0] = 'X';

	get("/api/v1/system/coredump");
	zassert_equal(ctx.rsp.status, 200, "%s", body);
	zassert_str_equal(body, "{\"coredump\":{\"size_bytes\":100,\"reason\":null,"
				"\"reason_code\":null}}");
}

ZTEST(v1, test_coredump_downloads_every_byte_in_pieces)
{
	int pieces;
	int n;

	sign_in();
	install();
	store_dump(sizeof(dump), 0);

	get("/api/v1/system/coredump/data");
	zassert_equal(ctx.rsp.status, 200, "%s", body);
	zassert_str_equal(response_header("Content-Type"), "application/octet-stream");
	zassert_str_equal(response_header("Content-Disposition"),
			  "attachment; filename=\"cedar-coredump.bin\"");
	zassert_true(web_api_stream_active(&ctx));
	n = pull_all(&pieces);
	zassert_equal(n, (int)sizeof(dump));
	zassert_true(pieces > 1, "%d pieces", pieces);
	zassert_mem_equal(downloaded, dump, sizeof(dump));
}

ZTEST(v1, test_coredump_read_failure_cuts_the_download)
{
	int pieces;

	sign_in();
	install();
	store_dump(sizeof(dump), 0);
	read_error_at = 20000;

	get("/api/v1/system/coredump/data");
	zassert_equal(ctx.rsp.status, 200, "%s", body);
	zassert_equal(pull_all(&pieces), -1, "an incomplete file must not look complete");
}

ZTEST(v1, test_coredump_unreadable_storage_is_an_internal_error)
{
	sign_in();
	install();
	size_error = -EIO;

	get("/api/v1/system/coredump");
	expect_error(500, "internal_error");
	get("/api/v1/system/coredump/data");
	expect_error(500, "internal_error");
}

ZTEST(v1, test_coredump_clear)
{
	sign_in();
	install();
	store_dump(100, 3);

	clear(false);
	expect_error(403, "csrf_failed");
	zassert_equal(clears, 0);

	clear(true);
	zassert_equal(ctx.rsp.status, 204, "%s", body);
	zassert_equal(clears, 1);
	get("/api/v1/system/coredump");
	zassert_str_equal(body, "{\"coredump\":null}");

	/* Nothing stored: still 204. */
	clear(true);
	zassert_equal(ctx.rsp.status, 204, "%s", body);
}
