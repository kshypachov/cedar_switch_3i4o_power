/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Harness for the web-api suites.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <job_manager/job_manager.h>

#include "harness.h"

struct web_api_context ctx;
char body[CONFIG_WEB_API_RESPONSE_BODY_MAX + 1];
int64_t fake_now;
bool kdf_gate_enabled;
K_SEM_DEFINE(kdf_gate, 0, 1);
int kdf_calls;

static uint32_t random_counter;
static uint8_t store[128];
static size_t store_len;
static bool store_present;
static k_tid_t test_thread;
static char query_buf[256];
static char path_buf[256];

static int64_t clock_fn(void)
{
	return fake_now;
}

static int random_fn(uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		random_counter = random_counter * 1103515245U + 12345U;
		buf[i] = (uint8_t)(random_counter >> 16);
	}
	return 0;
}

static int kdf_fn(const uint8_t *password, size_t password_len, const uint8_t *salt,
		  size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
	uint64_t h = 0xcbf29ce484222325ULL ^ iterations;

	kdf_calls++;
	if (kdf_gate_enabled && k_current_get() != test_thread) {
		k_sem_take(&kdf_gate, K_FOREVER);
	}
	for (size_t i = 0; i < password_len; i++) {
		h = (h ^ password[i]) * 0x100000001b3ULL;
	}
	for (size_t i = 0; i < salt_len; i++) {
		h = (h ^ salt[i]) * 0x100000001b3ULL;
	}
	for (size_t i = 0; i < out_len; i++) {
		h = (h ^ i) * 0x100000001b3ULL;
		out[i] = (uint8_t)(h >> 29);
	}
	return 0;
}

static int load_fn(uint8_t *buf, size_t cap, size_t *out_len)
{
	if (!store_present) {
		return -ENOENT;
	}
	memcpy(buf, store, store_len);
	*out_len = store_len;
	return 0;
}

static int save_fn(const uint8_t *buf, size_t len)
{
	memcpy(store, buf, len);
	store_len = len;
	store_present = true;
	return 0;
}

const struct web_auth_platform fake_platform = {
	.random = random_fn,
	.now_ms = clock_fn,
	.kdf = kdf_fn,
	.load_verifier = load_fn,
	.save_verifier = save_fn,
};

void harness_reset(void)
{
	test_thread = k_current_get();
	fake_now = 1000;
	random_counter = 3;
	store_present = false;
	store_len = 0;
	kdf_calls = 0;
	kdf_gate_enabled = false;
	k_sem_reset(&kdf_gate);
	job_manager_init();
	zassert_ok(web_auth_init(&fake_platform));
}

void request(enum web_api_method method, const char *path)
{
	const char *q = strchr(path, '?');

	memset(&ctx.req, 0, sizeof(ctx.req));
	if (q != NULL) {
		size_t n = (size_t)(q - path);

		memcpy(path_buf, path, n);
		path_buf[n] = '\0';
		strcpy(query_buf, q + 1);
		ctx.req.path = path_buf;
		ctx.req.query = query_buf;
	} else {
		strcpy(path_buf, path);
		ctx.req.path = path_buf;
		ctx.req.query = "";
	}
	ctx.req.method = method;
	ctx.req.headers.host = "192.168.88.14";
	ctx.req.peer.family = 4;
	ctx.req.peer.addr[0] = 192;
	ctx.req.peer.addr[1] = 168;
	ctx.req.peer.addr[2] = 88;
	ctx.req.peer.addr[3] = 17;
}

void request_body(const char *json)
{
	size_t n = strlen(json);

	zassert_true(n <= sizeof(ctx.body));
	memcpy(ctx.body, json, n);
	ctx.req.body = ctx.body;
	ctx.req.body_len = n;
	ctx.req.body_received = n;
}

void dispatch(const struct web_api_router *router)
{
	web_api_dispatch(router, &ctx);
	memset(body, 0, sizeof(body));
	if (ctx.rsp.body != NULL) {
		memcpy(body, ctx.rsp.body, MIN(ctx.rsp.body_len, sizeof(body) - 1));
	}
	/* Every API response carries these, whatever the outcome. */
	zassert_not_null(response_header("X-Request-ID"));
	zassert_str_equal(response_header("Cache-Control"), "no-store");
	if (ctx.rsp.status >= 400) {
		char expected[96];

		snprintf(expected, sizeof(expected), "\"request_id\":\"%s\"",
			 response_header("X-Request-ID"));
		zassert_true(strstr(body, expected) != NULL,
			     "a rejection's request_id is its header's: %s", body);
		zassert_str_equal(response_header("Content-Type"), "application/json");
	}
}

const char *response_header(const char *name)
{
	for (size_t i = 0; i < ctx.rsp.header_count; i++) {
		if (strcmp(ctx.rsp.headers[i].name, name) == 0) {
			return ctx.rsp.headers[i].value;
		}
	}
	return NULL;
}

bool body_has(const char *fragment)
{
	return strstr(body, fragment) != NULL;
}
