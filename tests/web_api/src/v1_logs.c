/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The log and coprocessor bindings (src/web/api/v1/logs.c, coprocessor.c)
 * through the real route table, over the real log-store and
 * coprocessor-manager. Part of the v1 suite, whose before() resets web-auth,
 * job-manager, Matter and the network; each test here resets the store and
 * the manager itself.
 *
 * prj.conf gives the STM32 ring 64 KiB and the ESP32 ring 4 KiB, and a scan
 * budget of 32 records, so a wrap, a byte-bounded page and an exhausted budget
 * all take a few dozen records.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <log_store/log_store.h>

#include "harness.h"
#include "web_api_v1.h"

#define PASSWORD   "correct horse battery"
#define LAN_ORIGIN "http://192.168.88.14"
/* The identity v1.c gives web_api_v1_init(). */
#define BOOT_ID    "boot_test1"

static char session_cookie[96];

/* -- helpers ------------------------------------------------------------------ */

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

static void get_ok(const char *path)
{
	get(path);
	zassert_equal(ctx.rsp.status, 200, "%s: %s", path, body);
}

static void expect_error(const char *path, uint16_t status, const char *code)
{
	char fragment[64];

	get(path);
	snprintf(fragment, sizeof(fragment), "\"code\":\"%s\"", code);
	zassert_equal(ctx.rsp.status, status, "%s: %s", path, body);
	zassert_true(body_has(fragment), "%s: %s", path, body);
}

static void add_to(enum log_store_source source, enum log_store_level level, const char *module,
		   const char *text)
{
	const struct log_store_entry e = {
		.source = source,
		.level = level,
		.kind = LOG_STORE_KIND_MESSAGE,
		.generation = source == LOG_STORE_ESP32 ? 1U : 0U,
		.uptime_ms = (uint64_t)fake_now,
		.module = module,
		.module_len = module != NULL ? strlen(module) : 0U,
		.text = text,
		.text_len = strlen(text),
	};

	zassert_ok(log_store_append(&e));
}

static void add_lines(int n, const char *prefix)
{
	char text[48];

	for (int i = 0; i < n; i++) {
		snprintf(text, sizeof(text), "%s %d", prefix, i);
		add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "main", text);
	}
}

/* The "seq" values of @p text in order, into @p out; returns how many. */
static int seqs(const char *text, uint64_t *out, int cap)
{
	int n = 0;

	for (const char *p = strstr(text, "\"seq\":\""); p != NULL && n < cap;
	     p = strstr(p + 1, "\"seq\":\"")) {
		out[n++] = strtoull(p + 7, NULL, 10);
	}
	return n;
}

static int count(const char *text, const char *needle)
{
	int n = 0;

	for (const char *p = strstr(text, needle); p != NULL; p = strstr(p + 1, needle)) {
		n++;
	}
	return n;
}

static void cursor_of_page(char *out, size_t cap)
{
	zassert_true(json_string(body, "next_cursor", out, cap), "%s", body);
	zassert_true(out[0] != '\0');
}

/* -- a coprocessor platform that does what it is told --------------------------- */

static struct {
	uint32_t activity;
	bool transport;
} cp;

static int cp_attach(void *c, enum coprocessor_uart_mode owner)
{
	ARG_UNUSED(c);
	ARG_UNUSED(owner);
	return 0;
}

static int cp_detach(void *c)
{
	ARG_UNUSED(c);
	return 0;
}

static uint32_t cp_activity(void *c)
{
	ARG_UNUSED(c);
	return cp.activity;
}

static int cp_reset(void *c, bool download)
{
	ARG_UNUSED(c);
	ARG_UNUSED(download);
	return 0;
}

static bool cp_transport(void *c)
{
	ARG_UNUSED(c);
	return cp.transport;
}

static void cp_marker(void *c, enum log_store_kind kind, uint32_t generation, const char *text)
{
	const struct log_store_entry e = {
		.source = LOG_STORE_ESP32,
		.kind = kind,
		.generation = generation,
		.module = "coprocessor",
		.module_len = 11,
		.text = text,
		.text_len = strlen(text),
	};

	ARG_UNUSED(c);
	(void)log_store_append(&e);
}

static int64_t cp_now(void *c)
{
	ARG_UNUSED(c);
	return fake_now;
}

static void cp_sleep(void *c, uint32_t ms)
{
	ARG_UNUSED(c);
	fake_now += ms;
}

static const struct coprocessor_platform cp_platform = {
	.uart_attach = cp_attach,
	.uart_detach = cp_detach,
	.uart_rx_activity = cp_activity,
	.c6_reset = cp_reset,
	.transport_ready = cp_transport,
	.marker = cp_marker,
	.now_ms = cp_now,
	.sleep_ms = cp_sleep,
};

void v1_coprocessor_fake_init(void)
{
	memset(&cp, 0, sizeof(cp));
	zassert_ok(log_store_init());
	zassert_ok(coprocessor_manager_init(&cp_platform));
	/* The manager's init marker, if any, is not what these tests count. */
	zassert_ok(log_store_init());
	web_api_v1_set_coprocessor(NULL);
}

static void logs_reset(void)
{
	v1_coprocessor_fake_init();
	sign_in();
}

/* -- pages ------------------------------------------------------------------------ */

ZTEST(v1, test_logs_page_without_cursor_is_the_newest_oldest_first)
{
	uint64_t s[16];

	logs_reset();
	add_lines(30, "line");
	get_ok("/api/v1/logs/records?limit=5");

	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 5, "%s", body);
	for (int i = 0; i < 5; i++) {
		zassert_equal(s[i], 26U + i, "newest five, oldest first: %s", body);
	}
	zassert_true(body_has("\"boot_id\":\"" BOOT_ID "\""));
	zassert_true(body_has("\"has_more\":false"));
	zassert_true(body_has("\"gap\":false"));
	zassert_true(body_has("\"dropped_count\":\"0\""));
	zassert_true(body_has("\"wall_time\":null"));
	zassert_true(body_has("\"level\":\"info\""));
	zassert_true(body_has("\"module\":\"main\""));
	zassert_true(body_has("\"kind\":\"message\""));
	zassert_true(body_has("\"source_generation\":0"));
}

ZTEST(v1, test_logs_cursor_continues_and_has_more_is_exact)
{
	char c1[LOG_STORE_CURSOR_MAX];
	char c2[LOG_STORE_CURSOR_MAX];
	char c3[LOG_STORE_CURSOR_MAX];
	char again[LOG_STORE_CURSOR_MAX];
	char path[160];
	uint64_t s[16];

	logs_reset();
	add_lines(3, "first");
	get_ok("/api/v1/logs/records?limit=5");
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 3);
	cursor_of_page(c1, sizeof(c1));

	snprintf(path, sizeof(path), "/api/v1/logs/records?limit=5&cursor=%s", c1);
	get_ok(path);
	zassert_true(body_has("\"items\":[]"), "%s", body);
	zassert_true(body_has("\"has_more\":false"));
	cursor_of_page(again, sizeof(again));
	zassert_str_equal(again, c1, "resuming where nothing arrived is the same place");

	add_lines(5, "second");
	get_ok(path);
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 5);
	zassert_equal(s[0], 4U);
	zassert_true(body_has("\"has_more\":false"), "exactly limit new records: %s", body);
	cursor_of_page(c2, sizeof(c2));

	add_lines(6, "third");
	snprintf(path, sizeof(path), "/api/v1/logs/records?limit=5&cursor=%s", c2);
	get_ok(path);
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 5);
	zassert_true(body_has("\"has_more\":true"), "one more waits: %s", body);
	cursor_of_page(c3, sizeof(c3));

	snprintf(path, sizeof(path), "/api/v1/logs/records?limit=5&cursor=%s", c3);
	get_ok(path);
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 1);
	zassert_equal(s[0], 14U);
	zassert_true(body_has("\"has_more\":false"));
}

ZTEST(v1, test_logs_page_is_bounded_by_the_response_buffer)
{
	/* A control byte is six bytes of JSON: 500 of them are 3 KB per record. */
	char text[501];
	char cursor[LOG_STORE_CURSOR_MAX];
	char path[160];
	uint64_t all[64];
	int got = 0;

	logs_reset();
	memset(text, 0x01, 500);
	text[500] = '\0';
	for (int i = 0; i < 12; i++) {
		add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "burst", text);
	}

	get_ok("/api/v1/logs/records?limit=100");
	got = seqs(body, all, ARRAY_SIZE(all));
	zassert_true(got > 0 && got < 12, "the buffer ends the page: %d", got);
	zassert_true(body_has("\"has_more\":true"));
	zassert_true(ctx.rsp.body_len <= CONFIG_WEB_API_RESPONSE_BODY_MAX);

	for (int guard = 0; guard < 12 && body_has("\"has_more\":true"); guard++) {
		cursor_of_page(cursor, sizeof(cursor));
		snprintf(path, sizeof(path), "/api/v1/logs/records?limit=100&cursor=%s", cursor);
		get_ok(path);
		got += seqs(body, &all[got], ARRAY_SIZE(all) - got);
	}
	zassert_equal(got, 12, "every record once");
	for (int i = 0; i < got; i++) {
		zassert_equal(all[i], 1U + i, "in order, none twice");
	}
}

ZTEST(v1, test_logs_scan_budget_ends_a_page_that_matches_nothing)
{
	char cursor[LOG_STORE_CURSOR_MAX];
	char path[200];
	int pages = 0;

	logs_reset();
	get_ok("/api/v1/logs/records?module=rare&limit=10");
	cursor_of_page(cursor, sizeof(cursor));
	add_lines(80, "noise");
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_ERROR, "rare", "the one");

	do {
		snprintf(path, sizeof(path), "/api/v1/logs/records?module=rare&limit=10&cursor=%s",
			 cursor);
		get_ok(path);
		cursor_of_page(cursor, sizeof(cursor));
		pages++;
		if (!body_has("the one")) {
			zassert_true(body_has("\"items\":[]"), "%s", body);
			zassert_true(body_has("\"has_more\":true"),
				     "a scan cut short says there is more: %s", body);
		}
	} while (!body_has("the one") && pages < 10);

	zassert_true(body_has("the one"));
	zassert_equal(pages, 3, "81 records at 32 a page: %d", pages);
	zassert_true(body_has("\"has_more\":false"));
}

ZTEST(v1, test_logs_cursor_behind_the_ring_reports_a_gap)
{
	char cursor[LOG_STORE_CURSOR_MAX];
	char path[160];
	uint64_t s[64];

	logs_reset();
	add_lines(2, "old");
	get_ok("/api/v1/logs/records?limit=1");
	cursor_of_page(cursor, sizeof(cursor));
	/* Well over 64 KiB of new records (about 50 bytes each): the ones after
	 * the cursor are gone. */
	add_lines(3000, "flood");

	snprintf(path, sizeof(path), "/api/v1/logs/records?limit=100&cursor=%s", cursor);
	get_ok(path);
	zassert_true(body_has("\"gap\":true"), "%s", body);
	zassert_true(seqs(body, s, ARRAY_SIZE(s)) > 0);
	zassert_true(s[0] > 2U, "continues at the oldest record kept");
}

ZTEST(v1, test_logs_cursor_of_another_boot_is_the_tail_with_a_gap)
{
	struct log_store_filter all = {.sources = LOG_STORE_SOURCES_ALL};
	struct log_store_reader r;
	char cursor[LOG_STORE_CURSOR_MAX];
	char path[160];
	uint64_t s[16];

	logs_reset();
	add_lines(20, "line");
	log_store_reader_tail(&r, &all, 0, 32);
	zassert_true(log_store_cursor_encode(&r, log_store_boot_tag("boot_before"), cursor,
					     sizeof(cursor)) > 0);

	snprintf(path, sizeof(path), "/api/v1/logs/records?limit=3&cursor=%s", cursor);
	get_ok(path);
	zassert_true(body_has("\"gap\":true"), "%s", body);
	zassert_true(body_has("\"boot_id\":\"" BOOT_ID "\""));
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 3);
	zassert_equal(s[0], 18U, "the newest three, as without a cursor");
}

ZTEST(v1, test_logs_bad_cursors_are_invalid_cursor)
{
	char cursor[LOG_STORE_CURSOR_MAX];
	char path[160];

	logs_reset();
	add_lines(4, "line");
	expect_error("/api/v1/logs/records?cursor=", 400, "invalid_cursor");
	expect_error("/api/v1/logs/records?cursor=not-a-real-cursor", 400, "invalid_cursor");
	expect_error("/api/v1/logs/records?cursor=0", 400, "invalid_cursor");

	get_ok("/api/v1/logs/records?source=esp32");
	cursor_of_page(cursor, sizeof(cursor));
	snprintf(path, sizeof(path), "/api/v1/logs/records?source=stm32&cursor=%s", cursor);
	expect_error(path, 400, "invalid_cursor");
	snprintf(path, sizeof(path), "/api/v1/logs/records?source=esp32&cursor=%s", cursor);
	get_ok(path);
}

ZTEST(v1, test_logs_query_values_outside_their_schema_are_invalid_query)
{
	char path[400];
	char many[200];

	logs_reset();
	expect_error("/api/v1/logs/records?limit=0", 400, "invalid_query");
	expect_error("/api/v1/logs/records?limit=101", 400, "invalid_query");
	expect_error("/api/v1/logs/records?limit=ten", 400, "invalid_query");
	expect_error("/api/v1/logs/records?limit=", 400, "invalid_query");
	expect_error("/api/v1/logs/records?source=uart", 400, "invalid_query");
	expect_error("/api/v1/logs/records?min_level=trace", 400, "invalid_query");
	expect_error("/api/v1/logs/records?levl=error", 400, "invalid_query");
	expect_error("/api/v1/logs/records?limit=5&limit=6", 400, "invalid_query");
	expect_error("/api/v1/logs/export?format=xml", 400, "invalid_query");
	expect_error("/api/v1/logs/export?max_records=0", 400, "invalid_query");
	expect_error("/api/v1/logs/export?max_records=2001", 400, "invalid_query");
	expect_error("/api/v1/logs/export?cursor=abc", 400, "invalid_query");

	memset(many, 'm', 65);
	many[65] = '\0';
	snprintf(path, sizeof(path), "/api/v1/logs/records?module=%s", many);
	expect_error(path, 400, "invalid_query");
	memset(many, 'c', 129);
	many[129] = '\0';
	snprintf(path, sizeof(path), "/api/v1/logs/records?contains=%s", many);
	expect_error(path, 400, "invalid_query");
	/* Checked before the cursor, in the mock's order. */
	expect_error("/api/v1/logs/records?limit=0&cursor=garbage", 400, "invalid_query");
}

ZTEST(v1, test_logs_filters)
{
	char path[256] = "/api/v1/logs/records?module=";
	uint64_t s[16];

	logs_reset();
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "net", "DHCP bound");
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_WARNING, "net", "link flapping");
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_NONE, NULL, "*** Booting ***");
	add_to(LOG_STORE_ESP32, LOG_STORE_LEVEL_UNKNOWN, NULL, "invalid header: 0xffffffff");
	add_to(LOG_STORE_ESP32, LOG_STORE_LEVEL_DEBUG, "wifi", "scan done");

	get_ok("/api/v1/logs/records?min_level=warning");
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 3, "warning, null and unknown: %s", body);
	zassert_true(body_has("\"level\":null"));
	zassert_true(body_has("\"module\":null"));
	zassert_true(body_has("\"level\":\"unknown\""));

	get_ok("/api/v1/logs/records?source=esp32");
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 2);
	zassert_true(body_has("\"source_generation\":1"));

	get_ok("/api/v1/logs/records?module=net&contains=dhcp");
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 1, "%s", body);
	zassert_equal(s[0], 1U);

	get_ok("/api/v1/logs/records?contains=Booting+%2A%2A%2A");
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 1, "form-decoded: %s", body);

	/* 33 Cyrillic letters: 33 code points, 66 bytes - valid, and longer than
	 * any module a record can carry. */
	for (int i = 0; i < 33; i++) {
		strcat(path, "%D0%B6");
	}
	get_ok(path);
	zassert_true(body_has("\"items\":[]"), "%s", body);
}

/* -- sources, capabilities and the UART's owner ----------------------------------- */

ZTEST(v1, test_logs_sources_follow_the_uart_owner_not_the_transport)
{
	logs_reset();
	get_ok("/api/v1/logs/sources");
	zassert_true(body_has("{\"id\":\"stm32\",\"available\":true,\"reason\":null,"
			      "\"generation\":0,\"dropped_count\":\"0\"}"),
		     "%s", body);
	zassert_true(body_has("{\"id\":\"esp32\",\"available\":true,\"reason\":null,"
			      "\"generation\":1,\"dropped_count\":\"0\"}"),
		     "no transport, and the console still reads the UART: %s", body);
	get_ok("/api/v1/capabilities");
	zassert_true(body_has("\"esp32_logs\":{\"available\":true,\"reason\":null}"), "%s", body);
	zassert_true(body_has("\"esp32_uart\":{\"available\":false,\"reason\":\"not_implemented\"}"));

	zassert_ok(coprocessor_manager_set_mode(COPROCESSOR_UART_USB_BRIDGE));
	get_ok("/api/v1/logs/sources");
	zassert_true(body_has("\"id\":\"esp32\",\"available\":false,\"reason\":\"uart_usb_bridge\""),
		     "%s", body);
	get_ok("/api/v1/capabilities");
	zassert_true(body_has("\"esp32_logs\":{\"available\":false,\"reason\":\"uart_usb_bridge\"}"),
		     "%s", body);

	zassert_ok(coprocessor_manager_set_mode(COPROCESSOR_UART_CONSOLE));
	get_ok("/api/v1/logs/sources");
	zassert_true(body_has("\"id\":\"esp32\",\"available\":true,\"reason\":null,"
			      "\"generation\":2"),
		     "the UART came back: a new generation: %s", body);

	log_store_count_loss(LOG_STORE_ESP32, LOG_STORE_LOSS_UART, 3);
	log_store_count_loss(LOG_STORE_STM32, LOG_STORE_LOSS_BACKEND, 2);
	get_ok("/api/v1/logs/sources");
	zassert_true(body_has("\"generation\":0,\"dropped_count\":\"2\""), "%s", body);
	zassert_true(body_has("\"generation\":2,\"dropped_count\":\"3\""), "%s", body);
	get_ok("/api/v1/logs/records?source=esp32");
	zassert_true(body_has("\"dropped_count\":\"3\""), "%s", body);
	get_ok("/api/v1/logs/records");
	zassert_true(body_has("\"dropped_count\":\"5\""), "%s", body);
}

ZTEST(v1, test_coprocessor_status)
{
	logs_reset();
	get_ok("/api/v1/coprocessor/status");
	zassert_true(body_has("\"chip\":\"esp32c6\""));
	zassert_true(body_has("\"firmware_version\":null"));
	zassert_true(body_has("\"host_protocol\":null"));
	zassert_true(body_has("\"partition_layout_id\":null"));
	zassert_true(body_has("\"transport_ready\":false"));
	zassert_true(body_has("\"uart_mode\":\"console\""));
	zassert_true(body_has("\"generation\":1"));
	zassert_true(body_has("\"ota\":{\"available\":false,\"reason\":\"not_implemented\"}"));
	zassert_true(body_has("\"uart_update\":{\"available\":false,\"reason\":\"not_implemented\"}"));
	zassert_true(body_has("\"last_update\":null"));

	cp.transport = true;
	get_ok("/api/v1/coprocessor/status");
	zassert_true(body_has("\"state\":\"ready\""), "%s", body);
	zassert_true(body_has("\"host_protocol\":\"esp-hosted-mcu\""));

	zassert_ok(coprocessor_manager_set_mode(COPROCESSOR_UART_FLASHING));
	get_ok("/api/v1/coprocessor/status");
	zassert_true(body_has("\"state\":\"updating\""), "%s", body);
	zassert_true(body_has("\"uart_mode\":\"flashing\""));
	zassert_ok(coprocessor_manager_set_mode(COPROCESSOR_UART_CONSOLE));
}

/* -- the export ---------------------------------------------------------------------- */

static char exported[256 * 1024];

/* Pull every piece of the stream the last dispatch started. */
static size_t pull_all(int *pieces)
{
	size_t n = 0;
	bool final = false;

	*pieces = 0;
	for (int guard = 0; !final && guard < 1000; guard++) {
		zassert_ok(web_api_stream_pull(&ctx, &final));
		if (!final) {
			zassert_true(ctx.rsp.body_len > 0, "no empty piece before the end");
			zassert_equal(ctx.rsp.body[ctx.rsp.body_len - 1], '\n', "whole lines per piece");
		}
		zassert_true(n + ctx.rsp.body_len < sizeof(exported));
		memcpy(&exported[n], ctx.rsp.body, ctx.rsp.body_len);
		n += ctx.rsp.body_len;
		*pieces += ctx.rsp.body_len > 0 ? 1 : 0;
	}
	zassert_true(final);
	zassert_false(web_api_stream_active(&ctx));
	exported[n] = '\0';
	return n;
}

ZTEST(v1, test_logs_export_ndjson)
{
	uint64_t s[16];
	int pieces;

	logs_reset();
	add_lines(10, "line");
	get("/api/v1/logs/export?max_records=5");
	zassert_equal(ctx.rsp.status, 200);
	zassert_str_equal(response_header("Content-Type"), "application/x-ndjson");
	zassert_str_equal(response_header("Content-Disposition"),
			  "attachment; filename=\"cedar-logs.ndjson\"");
	zassert_true(web_api_stream_active(&ctx));

	pull_all(&pieces);
	zassert_equal(count(exported, "\n"), 5, "%s", exported);
	zassert_equal(seqs(exported, s, ARRAY_SIZE(s)), 5);
	zassert_equal(s[0], 6U, "the newest five");
	zassert_equal(pieces, 1);
	zassert_true(strstr(exported, "{\"source\":\"stm32\",\"boot_id\":\"" BOOT_ID "\"") ==
		     exported);
}

ZTEST(v1, test_logs_export_text)
{
	int pieces;

	logs_reset();
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "main", "two\nlines");
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_NONE, NULL, "printk");
	get("/api/v1/logs/export?format=text");
	zassert_equal(ctx.rsp.status, 200);
	zassert_str_equal(response_header("Content-Type"), "text/plain; charset=utf-8");
	zassert_str_equal(response_header("Content-Disposition"),
			  "attachment; filename=\"cedar-logs.txt\"");
	pull_all(&pieces);

	zassert_true(strncmp(exported, "# cedar logs " BOOT_ID ":", 20) == 0, "%s", exported);
	zassert_not_null(strstr(exported, " stm32 info main two\\nlines\n"), "%s", exported);
	zassert_not_null(strstr(exported, " stm32 - - printk\n"), "%s", exported);
	zassert_equal(count(exported, "\n"), 3);
}

ZTEST(v1, test_logs_export_is_streamed_in_pieces)
{
	char text[401];
	int pieces;

	logs_reset();
	memset(text, 'x', 400);
	text[400] = '\0';
	for (int i = 0; i < 100; i++) {
		add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "burst", text);
	}
	get("/api/v1/logs/export");
	const size_t n = pull_all(&pieces);

	zassert_true(n > CONFIG_WEB_API_RESPONSE_BODY_MAX, "%zu", n);
	zassert_true(pieces >= 3, "one response buffer per piece: %d", pieces);
	zassert_equal(count(exported, "\n"), 100);
}

ZTEST(v1, test_logs_export_marks_what_the_ring_overwrote_meanwhile)
{
	char text[401];
	const uint64_t first_new = 101;
	uint64_t s[512];
	bool final;
	int n;

	logs_reset();
	memset(text, 'y', 400);
	text[400] = '\0';
	for (int i = 0; i < 100; i++) {
		add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "burst", text);
	}
	get("/api/v1/logs/export");
	zassert_ok(web_api_stream_pull(&ctx, &final));
	zassert_false(final);

	/* A browser that reads slowly: the rest of the snapshot is overwritten. */
	for (int i = 0; i < 200; i++) {
		add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "later", text);
	}
	int pieces;

	pull_all(&pieces);
	zassert_not_null(strstr(exported, "\"kind\":\"gap\""), "%s", exported);
	zassert_not_null(strstr(exported, "records were overwritten before they could be exported"));
	zassert_is_null(strstr(exported, "later"), "nothing logged after the export started");

	/* Every record is from the snapshot. The ring took everything after the
	 * first piece, so the gap is last and carries the snapshot's upper bound:
	 * the first sequence number after the gap. */
	int gaps = 0;

	for (char *line = exported; *line != '\0';) {
		char *end = strchr(line, '\n');

		zassert_not_null(end);
		*end = '\0';
		zassert_equal(seqs(line, s, 1), 1, "%s", line);
		if (strstr(line, "\"kind\":\"gap\"") != NULL) {
			gaps++;
			zassert_equal(s[0], first_new, "%s", line);
			zassert_true(strstr(line, "\"level\":null,\"module\":null") != NULL, "%s", line);
			zassert_not_null(strstr(line, "\"source\":\"stm32\""),
					 "the first source the export selects: %s", line);
			zassert_equal(end[1], '\0', "the gap is the last line");
		} else {
			zassert_true(s[0] < first_new, "%s", line);
		}
		line = end + 1;
	}
	zassert_equal(gaps, 1);
	n = 0;
	ARG_UNUSED(n);
}

ZTEST(v1, test_logs_export_abandoned_midway_ends_its_stream)
{
	char text[401];
	bool final;

	logs_reset();
	memset(text, 'z', 400);
	text[400] = '\0';
	for (int i = 0; i < 100; i++) {
		add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "burst", text);
	}
	get("/api/v1/logs/export");
	zassert_ok(web_api_stream_pull(&ctx, &final));
	zassert_false(final);
	web_api_stream_end(&ctx);
	zassert_false(web_api_stream_active(&ctx));

	/* The context answers the next request normally. */
	get_ok("/api/v1/logs/records?limit=1");
	zassert_false(web_api_stream_active(&ctx));
	zassert_equal(seqs(body, (uint64_t[1]){0}, 1), 1);
}

ZTEST(v1, test_logs_export_of_an_empty_store)
{
	int pieces;

	logs_reset();
	get("/api/v1/logs/export");
	zassert_equal(ctx.rsp.status, 200);
	zassert_equal(pull_all(&pieces), 0U);
	zassert_equal(pieces, 0);

	get("/api/v1/logs/export?format=text");
	pull_all(&pieces);
	zassert_equal(count(exported, "\n"), 1, "only the warning line: %s", exported);
}

ZTEST(v1, test_logs_a_new_request_ends_a_stream_left_open)
{
	char text[401];
	bool final;

	logs_reset();
	memset(text, 'q', 400);
	text[400] = '\0';
	for (int i = 0; i < 100; i++) {
		add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "burst", text);
	}
	get("/api/v1/logs/export");
	zassert_ok(web_api_stream_pull(&ctx, &final));
	zassert_false(final);

	/* No end() in between: the context is reused as the adapter would after a
	 * client went quiet without the server noticing. */
	get_ok("/api/v1/logs/sources");
	zassert_false(web_api_stream_active(&ctx), "the old export ended with the new request");
	zassert_ok(web_api_stream_pull(&ctx, &final));
	zassert_true(final, "nothing of the old export follows");
	zassert_equal(ctx.rsp.body_len, 0U);
}

/* -- found by mutation (reports/p5/notes-mutations-web-api.md) ------------------ */

ZTEST(v1, test_logs_integer_query_values_are_digits_only)
{
	logs_reset();
	/* "0a" would read as 0*10 + ('a' - '0') = 49 without the digit check. */
	expect_error("/api/v1/logs/records?limit=0a", 400, "invalid_query");
	expect_error("/api/v1/logs/export?max_records=1b", 400, "invalid_query");
	get_ok("/api/v1/logs/records?limit=100");
	get_ok("/api/v1/logs/records?limit=1");
}

ZTEST(v1, test_logs_module_filter_at_its_limits)
{
	char module[65];
	/* The Cyrillic module below is 41 + 198 characters. */
	char path[300];
	uint64_t s[4];

	logs_reset();
	memset(module, 'm', 64);
	module[64] = '\0';
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, module, "a module of exactly 64 bytes");
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "other", "not this one");

	snprintf(path, sizeof(path), "/api/v1/logs/records?module=%s", module);
	get_ok(path);
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 1, "64 characters and 64 bytes still match: %s",
		      body);

	/* 33 Cyrillic letters: valid, longer than any stored module, and the page
	 * still reports the selected sources' losses. */
	strcpy(path, "/api/v1/logs/records?source=stm32&module=");
	for (int i = 0; i < 33; i++) {
		strcat(path, "%D0%B6");
	}
	log_store_count_loss(LOG_STORE_STM32, LOG_STORE_LOSS_BACKEND, 2);
	get_ok(path);
	zassert_true(body_has("\"items\":[]"), "%s", body);
	zassert_true(body_has("\"dropped_count\":\"2\""), "%s", body);
}

ZTEST(v1, test_logs_truncated_records_and_source_stm32)
{
	const struct log_store_entry cut = {
		.source = LOG_STORE_STM32,
		.level = LOG_STORE_LEVEL_WARNING,
		.truncated = true,
		.module = "net",
		.module_len = 3,
		.text = "a line that was cut",
		.text_len = 19,
	};
	uint64_t s[8];

	logs_reset();
	zassert_ok(log_store_append(&cut));
	add_to(LOG_STORE_ESP32, LOG_STORE_LEVEL_UNKNOWN, NULL, "rom");
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "net", "whole");

	get_ok("/api/v1/logs/records?source=stm32");
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 2, "%s", body);
	zassert_true(body_has("\"message\":\"a line that was cut\",\"truncated\":true"), "%s", body);
	zassert_true(body_has("\"message\":\"whole\",\"truncated\":false"), "%s", body);
	zassert_false(body_has("\"source\":\"esp32\""));
}

ZTEST(v1, test_logs_scan_budget_is_spent_on_the_page_not_the_tail)
{
	char cursor[LOG_STORE_CURSOR_MAX];
	char path[160];
	uint64_t s[64];

	/* 30 records and a budget of 32: a tail of 30 that also counted its own
	 * walk against the page would return two. */
	logs_reset();
	add_lines(30, "line");
	get_ok("/api/v1/logs/records?limit=30");
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 30, "%s", body);
	zassert_true(body_has("\"has_more\":false"));

	/* 40 new records after a cursor: the budget ends the page after 32, with
	 * more to come. */
	cursor_of_page(cursor, sizeof(cursor));
	add_lines(40, "more");
	snprintf(path, sizeof(path), "/api/v1/logs/records?limit=100&cursor=%s", cursor);
	get_ok(path);
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 32, "%s", body);
	zassert_true(body_has("\"has_more\":true"), "%s", body);
}

ZTEST(v1, test_logs_export_marks_a_gap_between_records)
{
	char text[401];
	uint64_t s[1];
	bool final;
	int gaps = 0;
	int after_gap = 0;
	int pieces;

	logs_reset();
	memset(text, 'g', 400);
	text[400] = '\0';
	/* About 62 KiB of the 64 KiB ring. */
	for (int i = 0; i < 140; i++) {
		add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "burst", text);
	}
	get("/api/v1/logs/export");
	zassert_ok(web_api_stream_pull(&ctx, &final));
	zassert_false(final);
	/* Overwrites the part just after the first piece, not the whole snapshot. */
	for (int i = 0; i < 40; i++) {
		add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "later", text);
	}
	pull_all(&pieces);

	for (char *line = exported; *line != '\0';) {
		char *end = strchr(line, '\n');

		zassert_not_null(end);
		*end = '\0';
		zassert_equal(seqs(line, s, 1), 1, "%s", line);
		if (strstr(line, "\"kind\":\"gap\"") != NULL) {
			gaps++;
			zassert_not_null(strstr(line, "\"source\":\"stm32\""), "%s", line);
			/* The gap names the first record after it. */
			zassert_not_equal(end[1], '\0', "records follow the gap");
			uint64_t next;

			zassert_equal(seqs(end + 1, &next, 1), 1);
			zassert_equal(s[0], next, "%s", line);
		} else if (gaps > 0) {
			after_gap++;
			zassert_true(s[0] <= 140U, "only the snapshot: %s", line);
		}
		line = end + 1;
	}
	zassert_equal(gaps, 1, "one gap, not one per record after it");
	zassert_true(after_gap > 50, "the kept part of the snapshot follows: %d", after_gap);
}

ZTEST(v1, test_logs_export_finds_a_rare_match_past_the_scan_budget)
{
	int pieces;

	logs_reset();
	add_lines(100, "noise");
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_ERROR, "rare", "the only match");
	get("/api/v1/logs/export?module=rare");
	pull_all(&pieces);
	zassert_equal(count(exported, "\n"), 1, "%s", exported);
	zassert_not_null(strstr(exported, "the only match"));
}

ZTEST(v1, test_logs_export_text_writes_every_line_break_as_an_escape)
{
	int pieces;

	logs_reset();
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "main", "a\rb");
	add_to(LOG_STORE_STM32, LOG_STORE_LEVEL_INFO, "main", "c\r\nd");
	get("/api/v1/logs/export?format=text");
	pull_all(&pieces);
	zassert_not_null(strstr(exported, " main a\\nb\n"), "%s", exported);
	zassert_not_null(strstr(exported, " main c\\nd\n"), "CRLF is one break: %s", exported);
	zassert_equal(count(exported, "\n"), 3);
}

static bool fw_known;
static bool rx_heard;

static bool fake_fw(char *buf, size_t cap)
{
	if (fw_known) {
		snprintf(buf, cap, "v3.0.6");
	}
	return fw_known;
}

static bool fake_rx(void)
{
	return rx_heard;
}

ZTEST(v1, test_coprocessor_state_and_version_follow_the_transport)
{
	static const struct web_api_v1_coprocessor hooks = {
		.firmware_version = fake_fw,
		.rx_seen = fake_rx,
	};

	logs_reset();
	web_api_v1_set_coprocessor(&hooks);
	fw_known = true;
	rx_heard = false;

	if (k_uptime_get() < 20000) {
		get_ok("/api/v1/coprocessor/status");
		zassert_true(body_has("\"state\":\"starting\""), "%s", body);
		k_sleep(K_MSEC(20001 - k_uptime_get()));
	}
	get_ok("/api/v1/coprocessor/status");
	zassert_true(body_has("\"state\":\"offline\""), "nothing heard: %s", body);
	zassert_true(body_has("\"firmware_version\":null"), "no transport, no version: %s", body);

	rx_heard = true;
	get_ok("/api/v1/coprocessor/status");
	zassert_true(body_has("\"state\":\"failed\""), "the chip talks, no transport: %s", body);

	cp.transport = true;
	get_ok("/api/v1/coprocessor/status");
	zassert_true(body_has("\"state\":\"ready\""), "%s", body);
	zassert_true(body_has("\"firmware_version\":\"v3.0.6\""), "%s", body);

	fw_known = false;
	get_ok("/api/v1/coprocessor/status");
	zassert_true(body_has("\"firmware_version\":null"), "%s", body);
	web_api_v1_set_coprocessor(NULL);
}

ZTEST(v1, test_logs_an_empty_contains_is_no_filter)
{
	char cursor[LOG_STORE_CURSOR_MAX];
	char path[160];
	uint64_t s[8];

	/* As the mock reads it: `contains=` selects what no `contains` selects, and a
	 * cursor from one continues under the other. */
	logs_reset();
	add_lines(3, "line");
	get_ok("/api/v1/logs/records?limit=2");
	cursor_of_page(cursor, sizeof(cursor));
	add_lines(2, "more");
	snprintf(path, sizeof(path), "/api/v1/logs/records?contains=&cursor=%s", cursor);
	get_ok(path);
	zassert_equal(seqs(body, s, ARRAY_SIZE(s)), 2, "%s", body);
}
