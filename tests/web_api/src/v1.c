/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The v1 bindings the device serves, through the real route table, with
 * web-auth running on a fake platform and job-manager for real.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <job_manager/job_manager.h>
#include <device_config_store/device_config_store.h>
#include <matter_service/matter_service.h>
#include <network_manager/network_manager.h>

#include "fake_iface.h"
#include "fake_storage.h"
#include "harness.h"
#include "web_api_v1.h"

#define PASSWORD     "correct horse battery"
#define NEW_PASSWORD "another good password"
#define LAN_ORIGIN   "http://192.168.88.14"

static const struct web_api_v1_identity identity = {
	.device_id = "cedar-0011aabb",
	.model = "cedar_switch_3in4out_power",
	.firmware_version = "test-firmware",
	.frontend_version = "test-frontend",
	.boot_id = "boot_test1",
};

static char cookie[96];
static char csrf[WEB_AUTH_TOKEN_LEN + 1];

/* Copy the JSON string value of "name" from the body. */
static bool body_string(const char *name, char *out, size_t cap)
{
	char key[48];
	const char *p;
	const char *end;

	snprintf(key, sizeof(key), "\"%s\":\"", name);
	p = strstr(body, key);
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

static void send_v1(void)
{
	dispatch(&web_api_v1_router);
}

static void remember_session(void)
{
	const char *set_cookie = response_header("Set-Cookie");
	const char *semi;

	zassert_not_null(set_cookie);
	semi = strchr(set_cookie, ';');
	zassert_not_null(semi);
	memcpy(cookie, set_cookie, semi - set_cookie);
	cookie[semi - set_cookie] = '\0';
	zassert_true(body_string("csrf_token", csrf, sizeof(csrf)));
}

static void get(const char *path)
{
	request(WEB_API_GET, path);
	ctx.req.headers.cookie = cookie;
	send_v1();
}

static void setup_device(void)
{
	char token[WEB_AUTH_SETUP_TOKEN_LEN + 1];

	request(WEB_API_GET, "/api/v1/auth/state");
	send_v1();
	zassert_true(body_string("setup_token", token, sizeof(token)));

	request(WEB_API_POST, "/api/v1/auth/setup");
	ctx.req.headers.origin = LAN_ORIGIN;
	ctx.req.headers.setup_token = token;
	request_body("{\"password\":\"" PASSWORD "\"}");
	send_v1();
	zassert_equal(ctx.rsp.status, 201, "%s", body);
	remember_session();
}

static void login_with(const char *password)
{
	char json[128];

	snprintf(json, sizeof(json), "{\"password\":\"%s\"}", password);
	request(WEB_API_POST, "/api/v1/auth/session");
	ctx.req.headers.origin = LAN_ORIGIN;
	request_body(json);
	send_v1();
}

static void change_password(const char *current, const char *next, const char *key)
{
	char json[160];

	snprintf(json, sizeof(json), "{\"current_password\":\"%s\",\"new_password\":\"%s\"}",
		 current, next);
	request(WEB_API_PUT, "/api/v1/auth/password");
	ctx.req.headers.cookie = cookie;
	ctx.req.headers.csrf_token = csrf;
	ctx.req.headers.idempotency_key = key;
	request_body(json);
	send_v1();
}


/* -- a fake Matter stack for the Matter bindings ------------------------- */

struct matter_work {
	void (*fn)(void *arg);
	void *arg;
};

static struct {
	struct matter_work queue[4];
	size_t queued;
	struct matter_window_reading window;
	int open_result;
	struct matter_fabric table[2];
	size_t count;
	uint32_t min_s;
	uint32_t max_s;
} mf;

static int mf_schedule(void (*fn)(void *arg), void *arg)
{
	if (mf.queued == ARRAY_SIZE(mf.queue)) {
		return -ENOMEM;
	}
	mf.queue[mf.queued++] = (struct matter_work){fn, arg};
	return 0;
}

static int64_t mf_now(void)
{
	return fake_now;
}

static int mf_open(uint32_t timeout_seconds)
{
	ARG_UNUSED(timeout_seconds);
	if (mf.open_result == 0) {
		mf.window = (struct matter_window_reading){.open = true};
	}
	return mf.open_result;
}

static void mf_close(void)
{
	mf.window = (struct matter_window_reading){0};
}

static void mf_read_window(struct matter_window_reading *out)
{
	*out = mf.window;
}

static int mf_read_codes(struct matter_codes *out)
{
	strcpy(out->qr_payload, "MT:Y.K9042C00KA0648G00");
	strcpy(out->manual_pairing_code, "01234567890");
	strcpy(out->setup_passcode, "00012345");
	return 0;
}

static size_t mf_read_fabrics(struct matter_fabric *out, size_t max)
{
	size_t n = MIN(max, mf.count);

	memcpy(out, mf.table, n * sizeof(*out));
	return n;
}

static uint32_t mf_min(void)
{
	return mf.min_s;
}

static uint32_t mf_max(void)
{
	return mf.max_s;
}

static const struct matter_service_platform mf_platform = {
	.schedule = mf_schedule,
	.now_ms = mf_now,
	.open_basic_window = mf_open,
	.close_window = mf_close,
	.read_window = mf_read_window,
	.read_codes = mf_read_codes,
	.read_fabrics = mf_read_fabrics,
	.min_window_seconds = mf_min,
	.max_window_seconds = mf_max,
};

/* A stack that has not started: the state every v1 test begins in. */
static void matter_reset(void)
{
	memset(&mf, 0, sizeof(mf));
	mf.min_s = 180;
	mf.max_s = 900;
	zassert_ok(matter_service_init(&mf_platform));
}

static void matter_ready(void)
{
	matter_service_report_starting();
	matter_service_report_started(0);
}

/* Run what the service queued, as the Matter thread would. */
static void matter_drain(void)
{
	while (mf.queued > 0) {
		struct matter_work w = mf.queue[0];

		memmove(mf.queue, mf.queue + 1, (mf.queued - 1) * sizeof(mf.queue[0]));
		mf.queued--;
		w.fn(w.arg);
	}
}

static void matter_open_request(const char *json, const char *key)
{
	request(WEB_API_POST, "/api/v1/matter/commissioning");
	ctx.req.headers.cookie = cookie;
	ctx.req.headers.csrf_token = csrf;
	ctx.req.headers.idempotency_key = key;
	request_body(json);
	send_v1();
}

static void matter_close_request(const char *key)
{
	request(WEB_API_DELETE, "/api/v1/matter/commissioning");
	ctx.req.headers.cookie = cookie;
	ctx.req.headers.csrf_token = csrf;
	ctx.req.headers.idempotency_key = key;
	send_v1();
}

static void get_job(const char *job_id)
{
	char path[64];

	snprintf(path, sizeof(path), "/api/v1/jobs/%s", job_id);
	get(path);
	zassert_equal(ctx.rsp.status, 200, "%s", body);
}

/* -- network-manager over fake interfaces for the network bindings --------- */

static struct fake_storage net_storage;
static struct device_config_backend net_backend;
static struct fake_net net;
static struct network_iface_ops net_ops;
static int kicks;
/* Its own clock: moving the session clock past a confirmation deadline would
 * end the session the test is using. */
static int64_t net_now;

static int64_t net_clock(void)
{
	return net_now;
}

static void kick(void)
{
	kicks++;
}

static const struct web_api_v1_network net_hooks = {.kick = kick};

static void network_reset(void)
{
	net_now = 1000;
	fake_storage_init(&net_storage);
	fake_storage_bind(&net_storage, &net_backend);
	zassert_ok(device_config_init(&net_backend, NULL));
	fake_net_init(&net);
	fake_net_bind(&net, &net_ops);
	network_manager_set_clock(net_clock);
	zassert_ok(network_manager_init(&net_ops));
	web_api_v1_set_network(&net_hooks);
	kicks = 0;
}

static void *suite_setup(void)
{
	zassert_ok(web_api_v1_init(&identity));
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	harness_reset();
	matter_reset();
	network_reset();
	cookie[0] = '\0';
	csrf[0] = '\0';
}

static void after(void *f)
{
	ARG_UNUSED(f);
	/* Never leave the worker blocked in the fake derivation. */
	kdf_gate_enabled = false;
	k_sem_give(&kdf_gate);
}

ZTEST_SUITE(v1, NULL, suite_setup, before, after, NULL);

ZTEST(v1, test_every_route_is_routed)
{
	/* Each line of routes.h is reachable at a concrete URL: whatever the
	 * answer, it is never the 404 of a path nobody serves. */
	for (size_t i = 0; i < web_api_v1_router.route_count; i++) {
		const struct web_api_route *route = &web_api_v1_router.routes[i];
		char path[128] = WEB_API_BASE_PATH;
		const char *t = route->path;
		size_t n = strlen(path);

		while (*t != '\0' && n < sizeof(path) - 8) {
			if (*t == '{') {
				memcpy(&path[n], "x1", 2);
				n += 2;
				t = strchr(t, '}') + 1;
			} else {
				path[n++] = *t++;
			}
		}
		path[n] = '\0';
		request(route->method, path);
		send_v1();
		zassert_false(ctx.rsp.status == 404 && body_has("Not an API resource"),
			      "%s %s is not routed", route->operation_id, path);
	}
}

ZTEST(v1, test_auth_state_on_a_fresh_device)
{
	char token[64];

	request(WEB_API_GET, "/api/v1/auth/state");
	send_v1();
	zassert_equal(ctx.rsp.status, 200);
	zassert_true(body_string("setup_token", token, sizeof(token)));
	zassert_equal(strlen(token), WEB_AUTH_SETUP_TOKEN_LEN);
	char expected[128];

	snprintf(expected, sizeof(expected),
		 "{\"setup_required\":true,\"setup_allowed\":true,\"setup_token\":\"%s\"}", token);
	zassert_str_equal(body, expected);
}

ZTEST(v1, test_setup_signs_in_and_closes_setup)
{
	char expected[256];

	setup_device();
	zassert_str_equal(response_header("Location"), "/api/v1/auth/session");
	snprintf(expected, sizeof(expected), "%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%d",
		 cookie, CONFIG_WEB_AUTH_ABSOLUTE_TIMEOUT_SECONDS);
	zassert_str_equal(response_header("Set-Cookie"), expected);
	zassert_equal(strlen(cookie), strlen("cedar_session=") + WEB_AUTH_TOKEN_LEN);
	snprintf(expected, sizeof(expected),
		 "{\"username\":\"admin\",\"csrf_token\":\"%s\",\"idle_timeout_seconds\":%d,"
		 "\"absolute_remaining_seconds\":%d}",
		 csrf, CONFIG_WEB_AUTH_IDLE_TIMEOUT_SECONDS, CONFIG_WEB_AUTH_ABSOLUTE_TIMEOUT_SECONDS);
	zassert_str_equal(body, expected);
	zassert_false(body_has(cookie + strlen("cedar_session=")),
		      "the session token is never in a body");

	request(WEB_API_GET, "/api/v1/auth/state");
	send_v1();
	zassert_str_equal(body, "{\"setup_required\":false,\"setup_allowed\":false,"
				"\"setup_token\":null}");

	get("/api/v1/auth/session");
	zassert_equal(ctx.rsp.status, 200);
	zassert_true(body_has(csrf));
}

ZTEST(v1, test_setup_refusals)
{
	request(WEB_API_POST, "/api/v1/auth/setup");
	ctx.req.headers.setup_token = "0123456789abcdef0123456789abcdef";
	request_body("{\"password\":\"" PASSWORD "\"}");
	send_v1();
	zassert_equal(ctx.rsp.status, 403);
	zassert_true(body_has("\"code\":\"setup_not_allowed\""));

	request(WEB_API_POST, "/api/v1/auth/setup");
	ctx.req.headers.setup_token = "0123456789abcdef0123456789abcdef";
	request_body("{\"password\":\"short\"}");
	send_v1();
	zassert_equal(ctx.rsp.status, 422, "the schema is checked before the token");
	zassert_true(body_has("{\"path\":\"/password\",\"code\":\"out_of_range\"}"), "%s", body);

	setup_device();
	request(WEB_API_GET, "/api/v1/auth/state");
	send_v1();
	request(WEB_API_POST, "/api/v1/auth/setup");
	ctx.req.headers.setup_token = "0123456789abcdef0123456789abcdef";
	request_body("{\"password\":\"" NEW_PASSWORD "\"}");
	send_v1();
	zassert_equal(ctx.rsp.status, 403);
}

ZTEST(v1, test_login)
{
	login_with(PASSWORD);
	zassert_equal(ctx.rsp.status, 403, "no administrator yet");
	zassert_true(body_has("\"code\":\"setup_not_allowed\""));

	setup_device();
	login_with("not the password");
	zassert_equal(ctx.rsp.status, 401);
	zassert_true(body_has("\"code\":\"invalid_credentials\""));
	zassert_true(body_has("\"retryable\":false"));
	zassert_is_null(response_header("Set-Cookie"));

	login_with(PASSWORD);
	zassert_equal(ctx.rsp.status, 200);
	zassert_is_null(response_header("Location"));
	remember_session();
	get("/api/v1/auth/session");
	zassert_equal(ctx.rsp.status, 200);
}

ZTEST(v1, test_login_rate_limit_is_429_with_retry_after)
{
	setup_device();
	for (int i = 0; i < CONFIG_WEB_AUTH_FREE_FAILURES; i++) {
		login_with("wrong password");
		zassert_equal(ctx.rsp.status, 401);
	}
	login_with(PASSWORD);
	zassert_equal(ctx.rsp.status, 429, "%s", body);
	zassert_true(body_has("\"code\":\"rate_limited\""));
	zassert_true(body_has("\"retryable\":true"));
	zassert_str_equal(response_header("Retry-After"), "1");
}

ZTEST(v1, test_logout)
{
	setup_device();
	request(WEB_API_DELETE, "/api/v1/auth/session");
	ctx.req.headers.cookie = cookie;
	ctx.req.headers.csrf_token = csrf;
	send_v1();
	zassert_equal(ctx.rsp.status, 204);
	zassert_equal(ctx.rsp.body_len, 0);
	zassert_true(ctx.rsp.close_connection);
	zassert_str_equal(response_header("Set-Cookie"),
			  "cedar_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");

	get("/api/v1/auth/session");
	zassert_equal(ctx.rsp.status, 401);
	zassert_true(body_has("\"code\":\"session_expired\""));
}

static bool wait_for_terminal(const char *job_id, struct job_snapshot *job)
{
	for (int i = 0; i < 200; i++) {
		if (job_get(job_id, job) == 0 && job_state_is_terminal(job->state)) {
			return true;
		}
		k_sleep(K_MSEC(5));
	}
	return false;
}

ZTEST(v1, test_password_change_wrong_current_is_synchronous)
{
	setup_device();
	change_password("not the password", NEW_PASSWORD, "change-key-000000001");
	zassert_equal(ctx.rsp.status, 401, "%s", body);
	zassert_true(body_has("\"code\":\"invalid_credentials\""));
	zassert_equal(job_active_count(), 0, "a refused change creates no job");

	change_password(PASSWORD, NEW_PASSWORD, "change-key-000000001");
	zassert_equal(ctx.rsp.status, 202,
		      "the corrected retry under the same key is not answered with the refusal");
}

ZTEST(v1, test_password_change_policy)
{
	setup_device();
	change_password(PASSWORD, "short", "change-key-000000002");
	zassert_equal(ctx.rsp.status, 422);
	zassert_true(body_has("{\"path\":\"/new_password\",\"code\":\"out_of_range\"}"), "%s", body);
	zassert_equal(job_active_count(), 0);
}

ZTEST(v1, test_a_password_one_character_too_long_is_refused)
{
	char json[200];
	char password[130];

	memset(password, 'a', 129);
	password[129] = '\0';
	snprintf(json, sizeof(json), "{\"password\":\"%s\"}", password);
	request(WEB_API_POST, "/api/v1/auth/session");
	request_body(json);
	send_v1();
	zassert_equal(ctx.rsp.status, 422, "129 characters fit the buffer but not the schema: %s", body);
	zassert_true(body_has("{\"path\":\"/password\",\"code\":\"too_long\"}"), "%s", body);
}

ZTEST(v1, test_password_change_job)
{
	char job_id[JOB_ID_MAX_LEN + 1];
	char expected[256];
	char old_cookie[sizeof(cookie)];
	struct job_snapshot job;
	int calls;

	setup_device();
	kdf_gate_enabled = true;
	change_password(PASSWORD, NEW_PASSWORD, "change-key-000000003");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	snprintf(expected, sizeof(expected),
		 "{\"job_id\":\"%s\",\"job_url\":\"/api/v1/jobs/%s\","
		 "\"resource_url\":\"/api/v1/auth/session\"}",
		 job_id, job_id);
	zassert_str_equal(body, expected);
	snprintf(expected, sizeof(expected), "/api/v1/jobs/%s", job_id);
	zassert_str_equal(response_header("Location"), expected);
	zassert_str_equal(response_header("Retry-After"), "1");

	/* Held in the derivation: running, and visible in system status. */
	k_sleep(K_MSEC(20));
	snprintf(expected, sizeof(expected), "/api/v1/jobs/%s", job_id);
	get(expected);
	zassert_equal(ctx.rsp.status, 200);
	zassert_true(body_has("\"state\":\"running\""), "%s", body);
	zassert_true(body_has("\"phase\":\"hashing\""));
	get("/api/v1/system/status");
	snprintf(expected, sizeof(expected), "\"active_job_ids\":[\"%s\"]", job_id);
	zassert_true(body_has(expected), "%s", body);

	/* The same request again: the same job, no second verification. */
	calls = kdf_calls;
	change_password(PASSWORD, NEW_PASSWORD, "change-key-000000003");
	zassert_equal(ctx.rsp.status, 202);
	zassert_true(body_has(job_id));
	zassert_equal(kdf_calls, calls);

	/* The same key for a different request is a conflict. */
	change_password(PASSWORD, "yet another password", "change-key-000000003");
	zassert_equal(ctx.rsp.status, 409);
	zassert_true(body_has("\"code\":\"idempotency_conflict\""));

	/* A second change while one is in flight is busy. */
	change_password(PASSWORD, "yet another password", "change-key-000000004");
	zassert_equal(ctx.rsp.status, 409, "%s", body);
	zassert_true(body_has("\"code\":\"busy\""));

	memcpy(old_cookie, cookie, sizeof(old_cookie));
	kdf_gate_enabled = false;
	k_sem_give(&kdf_gate);
	zassert_true(wait_for_terminal(job_id, &job));
	zassert_equal(job.state, JOB_STATE_SUCCEEDED);

	get("/api/v1/auth/session");
	zassert_equal(ctx.rsp.status, 401, "every session ended");
	zassert_true(body_has("\"code\":\"session_expired\""));

	login_with(PASSWORD);
	zassert_equal(ctx.rsp.status, 401);
	login_with(NEW_PASSWORD);
	zassert_equal(ctx.rsp.status, 200);
	remember_session();

	snprintf(expected, sizeof(expected), "/api/v1/jobs/%s", job_id);
	get(expected);
	zassert_equal(ctx.rsp.status, 200);
	zassert_true(body_has("\"kind\":\"password_change\""));
	zassert_true(body_has("\"state\":\"succeeded\""));
	zassert_true(body_has("\"boot_id\":\"boot_test1\""));
	zassert_true(body_has("\"progress\":null"));
	zassert_true(body_has("\"cancellable\":false"));
	zassert_true(body_has("\"resource_url\":\"/api/v1/auth/session\""));
	zassert_true(body_has("\"error\":null"));
}

ZTEST(v1, test_password_change_when_jobs_are_exhausted)
{
	struct job_snapshot job;
	const struct job_create_params filler = {.kind = JOB_KIND_WIFI_SCAN};

	setup_device();
	for (int i = 0; i < CONFIG_JOB_MANAGER_MAX_JOBS; i++) {
		zassert_equal(job_create(&filler, &job), JOB_CREATE_NEW);
	}
	change_password(PASSWORD, NEW_PASSWORD, "change-key-000000005");
	zassert_equal(ctx.rsp.status, 429, "%s", body);
	zassert_str_equal(response_header("Retry-After"), "1");

	/* The held password was dropped: a later change is not busy. */
	zassert_ok(job_set_state(job.id, JOB_STATE_FAILED));
	change_password(PASSWORD, NEW_PASSWORD, "change-key-000000006");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
}

ZTEST(v1, test_system_status)
{
	struct job_snapshot job;
	const struct job_create_params p = {.kind = JOB_KIND_WIFI_SCAN};
	char expected[96];

	setup_device();
	get("/api/v1/system/status");
	zassert_equal(ctx.rsp.status, 200);
	zassert_true(body_has("{\"device_id\":\"cedar-0011aabb\",\"model\":\"cedar_switch_3in4out_power\","
			      "\"firmware_version\":\"test-firmware\",\"frontend_version\":\"test-frontend\","
			      "\"boot_id\":\"boot_test1\",\"uptime_ms\":\""), "%s", body);
	zassert_true(body_has("\",\"wall_time\":null,\"active_job_ids\":[]}"), "%s", body);

	zassert_equal(job_create(&p, &job), JOB_CREATE_NEW);
	get("/api/v1/system/status");
	snprintf(expected, sizeof(expected), "\"active_job_ids\":[\"%s\"]", job.id);
	zassert_true(body_has(expected), "%s", body);
	zassert_ok(job_set_state(job.id, JOB_STATE_SUCCEEDED));
	get("/api/v1/system/status");
	zassert_true(body_has("\"active_job_ids\":[]"), "finished jobs are not active");
}

ZTEST(v1, test_capabilities)
{
	setup_device();
	get("/api/v1/capabilities");
	zassert_equal(ctx.rsp.status, 200);
	zassert_str_equal(
		body,
		"{\"api_version\":\"1\",\"features\":{"
		"\"matter\":{\"available\":false,\"reason\":\"not_ready\"},"
		"\"esp32_logs\":{\"available\":false,\"reason\":\"not_implemented\"},"
		"\"esp32_ota\":{\"available\":false,\"reason\":\"not_implemented\"},"
		"\"esp32_uart\":{\"available\":false,\"reason\":\"not_implemented\"}},"
		"\"limits\":{\"json_body_bytes\":8192,\"upload_chunk_bytes\":16384,"
		"\"upload_max_bytes\":2097152,\"log_page_records\":100,\"scan_records\":64,"
		"\"commissioning_min_seconds\":180,\"commissioning_max_seconds\":900,"
		"\"network_confirm_min_seconds\":60,\"network_confirm_max_seconds\":300},"
		"\"wifi_security_modes\":[\"open\",\"wpa2_psk\",\"wpa3_sae\"],"
		"\"firmware_formats\":[\"raw_app\"],\"update_requires_ethernet\":true}");
}

ZTEST(v1, test_jobs)
{
	struct job_snapshot job;
	const struct job_create_params p = {.kind = JOB_KIND_UPLOAD_CHUNK, .cancellable = true};
	char path[64];

	setup_device();
	get("/api/v1/jobs/job_ffffffff");
	zassert_equal(ctx.rsp.status, 404);
	zassert_true(body_has("\"code\":\"not_found\""));

	get("/api/v1/jobs/not!an!id");
	zassert_equal(ctx.rsp.status, 404);

	zassert_equal(job_create(&p, &job), JOB_CREATE_NEW);
	snprintf(path, sizeof(path), "/api/v1/jobs/%s", job.id);

	get(path);
	zassert_true(body_has("\"state\":\"queued\",\"phase\":\"\",\"progress\":null,"
			      "\"cancellable\":true"), "%s", body);

	zassert_ok(job_set_state(job.id, JOB_STATE_RUNNING));
	zassert_ok(job_set_progress(job.id, 5, 10, true, JOB_PROGRESS_UNIT_BYTES));
	fake_now += 0; /* job-manager uses its own clock */
	get(path);
	zassert_true(body_has("\"progress\":{\"completed\":5,\"total\":10,\"unit\":\"bytes\"}"), "%s", body);

	zassert_ok(job_set_progress(job.id, 2, 0, false, JOB_PROGRESS_UNIT_RECORDS));
	get(path);
	zassert_true(body_has("\"progress\":{\"completed\":2,\"total\":null,\"unit\":\"steps\"}"), "%s", body);

	zassert_ok(job_fail(job.id, "busy", true));
	get(path);
	char expected[160];

	snprintf(expected, sizeof(expected),
		 "\"error\":{\"code\":\"busy\",\"message\":\"The operation failed\","
		 "\"request_id\":\"%s\",\"retryable\":true}",
		 response_header("X-Request-ID"));
	zassert_true(body_has(expected), "%s", body);
	zassert_true(body_has("\"resource_url\":null"));
	zassert_true(body_has("\"kind\":\"upload_chunk\""));
}

/* -- network ---------------------------------------------------------------- */

#define NET_IPV4_DHCP "{\"mode\":\"dhcp\",\"address\":null,\"prefix_length\":null,\"gateway\":null}"
#define NET_ETH_STATIC                                                                             \
	"{\"enabled\":true,\"ipv4\":{\"mode\":\"static\",\"address\":\"192.168.88.50\","            \
	"\"prefix_length\":24,\"gateway\":\"192.168.88.1\"}}"
#define NET_ETH_DHCP  "{\"enabled\":true,\"ipv4\":" NET_IPV4_DHCP "}"
#define NET_WIFI_OFF                                                                               \
	"{\"enabled\":false,\"ssid_base64\":\"\",\"security\":\"open\",\"hidden\":false,"           \
	"\"ipv4\":" NET_IPV4_DHCP ",\"credential\":{\"action\":\"keep\"}}"
#define NET_DNS_AUTO "{\"mode\":\"automatic\",\"servers\":[]}"

static char net_json[1024];

static const char *net_candidate(int revision, const char *dns, const char *eth, const char *wifi)
{
	snprintf(net_json, sizeof(net_json),
		 "{\"base_revision\":%d,\"config\":{\"preferred_interface\":\"ethernet\","
		 "\"dns\":%s,\"interfaces\":{\"ethernet\":%s,\"wifi\":%s}}}",
		 revision, dns, eth, wifi);
	return net_json;
}

static void mutate(enum web_api_method method, const char *path, const char *json, const char *key)
{
	request(method, path);
	ctx.req.headers.cookie = cookie;
	ctx.req.headers.csrf_token = csrf;
	ctx.req.headers.idempotency_key = key;
	if (json != NULL) {
		request_body(json);
	}
	send_v1();
}

static void stage_body(const char *json, const char *key)
{
	mutate(WEB_API_POST, "/api/v1/network/transactions", json, key);
}

/* Stage the static Ethernet candidate and apply it; returns with the transaction id. */
static void stage_and_apply(char *id, size_t cap, int timeout, const char *stage_key,
			    const char *apply_key)
{
	char path[160];
	char json[64];

	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC, NET_WIFI_OFF), stage_key);
	zassert_equal(ctx.rsp.status, 201, "%s", body);
	zassert_true(body_string("id", id, cap));
	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s/apply", id);
	snprintf(json, sizeof(json), "{\"confirmation_timeout_seconds\":%d}", timeout);
	mutate(WEB_API_POST, path, json, apply_key);
	zassert_equal(ctx.rsp.status, 202, "%s", body);
}

ZTEST(v1, test_network_status_and_config_are_different_resources)
{
	setup_device();
	net.eth.extra_count = 1;
	net.eth.extra[0] = (struct network_addr){
		.family = DEVICE_CONFIG_AF_INET6,
		.prefix_length = 64,
		.source = NETWORK_ADDR_LINK_LOCAL,
		.bytes = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x82, 0x34, 0x28, 0xff, 0xfe, 0x10, 0x6a, 0x1d},
	};
	network_manager_boot(NULL);
	zassert_true(network_manager_process() > 0);

	get("/api/v1/network/status");
	zassert_equal(ctx.rsp.status, 200, "%s", body);
	zassert_str_equal(
		body,
		"{\"interfaces\":[{\"id\":\"ethernet\",\"enabled\":true,\"link_up\":true,"
		"\"state\":\"ready\",\"mac_address\":\"80:34:28:10:6a:1d\",\"addresses\":["
		"{\"family\":\"ipv4\",\"address\":\"192.168.88.14\",\"prefix_length\":24,"
		"\"source\":\"dhcp\"},"
		"{\"family\":\"ipv6\",\"address\":\"fe80::8234:28ff:fe10:6a1d\",\"prefix_length\":64,"
		"\"source\":\"link_local\"}],\"ssid\":null,\"rssi_dbm\":null,\"error\":null},"
		"{\"id\":\"wifi\",\"enabled\":false,\"link_up\":false,\"state\":\"disabled\","
		"\"mac_address\":\"00:00:00:00:00:00\",\"addresses\":[],\"ssid\":null,"
		"\"rssi_dbm\":null,\"error\":null}],"
		"\"default_interface\":\"ethernet\",\"dns_servers\":[]}");

	get("/api/v1/network/config");
	zassert_equal(ctx.rsp.status, 200, "%s", body);
	zassert_str_equal(
		body,
		"{\"revision\":0,\"config\":{\"preferred_interface\":\"ethernet\","
		"\"dns\":{\"mode\":\"automatic\",\"servers\":[]},\"interfaces\":{"
		"\"ethernet\":{\"enabled\":true,\"ipv4\":" NET_IPV4_DHCP "},"
		"\"wifi\":{\"enabled\":false,\"ssid_base64\":\"\",\"security\":\"open\","
		"\"hidden\":false,\"ipv4\":" NET_IPV4_DHCP ",\"password_set\":false}}},"
		"\"pending_transaction_id\":null}");
}

ZTEST(v1, test_network_status_explains_an_absent_coprocessor)
{
	setup_device();
	net.wifi.present = false;

	get("/api/v1/network/status");
	zassert_equal(ctx.rsp.status, 200, "%s", body);
	zassert_true(body_has("{\"id\":\"wifi\",\"enabled\":false,\"link_up\":false,"
			      "\"state\":\"disabled\""),
		     "%s", body);
	zassert_true(body_has("\"error\":{\"code\":\"capability_unavailable\","
			      "\"message\":\"The Wi-Fi coprocessor does not respond\""),
		     "disabled, and still told why it cannot be enabled: %s", body);

	mutate(WEB_API_POST, "/api/v1/network/wifi/scans", "{}", "scan-key-000000000001");
	zassert_equal(ctx.rsp.status, 503, "%s", body);
	zassert_true(body_has("\"code\":\"capability_unavailable\""));
	zassert_equal(kicks, 0, "a refusal wakes nobody");
}

ZTEST(v1, test_network_staging_redacts_the_password_and_says_where_it_is)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	char expected[128];

	setup_device();
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC,
				 "{\"enabled\":true,\"ssid_base64\":\"Q2VkYXItTGFi\","
				 "\"security\":\"wpa2_psk\",\"hidden\":false,\"ipv4\":" NET_IPV4_DHCP
				 ",\"credential\":{\"action\":\"replace\","
				 "\"value\":\"sup3r-s3cret-pw\"}}"),
		   "stage-key-0000000001");
	zassert_equal(ctx.rsp.status, 201, "%s", body);
	zassert_false(body_has("sup3r-s3cret-pw"), "the password never comes back");
	zassert_true(body_has("\"ssid_base64\":\"Q2VkYXItTGFi\",\"security\":\"wpa2_psk\","
			      "\"hidden\":false"),
		     "%s", body);
	zassert_true(body_has("\"password_set\":true"), "%s", body);
	zassert_true(body_has("\"state\":\"staged\""));
	zassert_true(body_has("\"remaining_seconds\":300,\"reconnect_urls\":[],\"job_id\":null,"
			      "\"error\":null}"),
		     "%s", body);
	zassert_true(body_string("id", id, sizeof(id)));
	snprintf(expected, sizeof(expected), "/api/v1/network/transactions/%s", id);
	zassert_str_equal(response_header("Location"), expected);
	zassert_equal(kicks, 0, "staging leaves the worker alone");

	get("/api/v1/network/config");
	snprintf(expected, sizeof(expected), "\"pending_transaction_id\":\"%s\"", id);
	zassert_true(body_has(expected), "%s", body);
}

ZTEST(v1, test_network_staging_refusals_name_their_fields)
{
	setup_device();

	stage_body(net_candidate(0, NET_DNS_AUTO,
				 "{\"enabled\":true,\"ipv4\":{\"mode\":\"static\","
				 "\"address\":\"192.168.88.50\",\"prefix_length\":24,"
				 "\"gateway\":\"10.0.0.1\"}}",
				 NET_WIFI_OFF),
		   "stage-key-0000000002");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_true(body_has("\"fields\":[{\"path\":\"/config/interfaces/ethernet/ipv4/gateway\","
			      "\"code\":\"out_of_range\"}]"),
		     "%s", body);

	/* keep with a value: no branch of the oneOf, one entry on the object */
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC,
				 "{\"enabled\":false,\"ssid_base64\":\"\",\"security\":\"open\","
				 "\"hidden\":false,\"ipv4\":" NET_IPV4_DHCP ",\"credential\":"
				 "{\"action\":\"keep\",\"value\":\"stray\"}}"),
		   "stage-key-0000000003");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_true(body_has("\"fields\":[{\"path\":\"/config/interfaces/wifi/credential\","
			      "\"code\":\"conflicting\"}]"),
		     "%s", body);

	stage_body(net_candidate(0, "{\"mode\":\"manual\",\"servers\":[\"resolver\"]}",
				 NET_ETH_STATIC, NET_WIFI_OFF),
		   "stage-key-0000000004");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_true(body_has("\"fields\":[{\"path\":\"/config/dns/servers/0\","
			      "\"code\":\"conflicting\"}]"),
		     "%s", body);

	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC,
				 "{\"enabled\":true,\"ssid_base64\":\"not base64!\","
				 "\"security\":\"open\",\"hidden\":false,\"ipv4\":" NET_IPV4_DHCP
				 ",\"credential\":{\"action\":\"clear\"}}"),
		   "stage-key-0000000005");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_true(body_has("\"fields\":[{\"path\":\"/config/interfaces/wifi/ssid_base64\","
			      "\"code\":\"invalid_format\"}]"),
		     "%s", body);

	stage_body(net_candidate(7, NET_DNS_AUTO, NET_ETH_STATIC, NET_WIFI_OFF),
		   "stage-key-0000000006");
	zassert_equal(ctx.rsp.status, 409, "%s", body);
	zassert_true(body_has("\"code\":\"stale_revision\""));
	zassert_equal(kicks, 0);
}

ZTEST(v1, test_network_staging_is_replayed_by_its_key)
{
	char first[NETWORK_TXN_ID_MAX_LEN + 1];
	char again[NETWORK_TXN_ID_MAX_LEN + 1];

	setup_device();
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC, NET_WIFI_OFF),
		   "stage-key-0000000007");
	zassert_equal(ctx.rsp.status, 201, "%s", body);
	zassert_true(body_string("id", first, sizeof(first)));

	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC, NET_WIFI_OFF),
		   "stage-key-0000000007");
	zassert_equal(ctx.rsp.status, 201, "a lost response is not answered with busy: %s", body);
	zassert_true(body_string("id", again, sizeof(again)));
	zassert_str_equal(first, again);

	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_DHCP, NET_WIFI_OFF),
		   "stage-key-0000000007");
	zassert_equal(ctx.rsp.status, 409, "%s", body);
	zassert_true(body_has("\"code\":\"idempotency_conflict\""));

	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_DHCP, NET_WIFI_OFF),
		   "stage-key-0000000008");
	zassert_equal(ctx.rsp.status, 409, "%s", body);
	zassert_true(body_has("\"code\":\"busy\""), "a second candidate is still refused");
}

ZTEST(v1, test_network_apply_confirm_and_commit)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char path[160];
	char job_member[64];

	setup_device();
	stage_and_apply(id, sizeof(id), 120, "flow-key-00000000001", "flow-key-00000000002");
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	zassert_true(body_has("\"resource_url\":\"/api/v1/network/config\""), "%s", body);
	zassert_equal(kicks, 1, "the worker is woken to change the interfaces");
	zassert_equal(net.eth.configure_calls, 0, "and nothing changed before the response");
	snprintf(job_member, sizeof(job_member), "\"job_id\":\"%s\"", job_id);

	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s", id);
	get(path);
	zassert_true(body_has("\"state\":\"applying\""), "%s", body);
	zassert_true(body_has("\"remaining_seconds\":120,"
			      "\"reconnect_urls\":[\"http://192.168.88.50/\"]"),
		     "%s", body);
	zassert_true(body_has(job_member), "%s", body);

	zassert_true(network_manager_process() > 0);
	get(path);
	zassert_true(body_has("\"state\":\"awaiting_confirmation\""), "%s", body);

	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s/confirm", id);
	mutate(WEB_API_POST, path, "{}", "flow-key-00000000003");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_has(job_member), "confirm answers with the apply job: %s", body);
	zassert_equal(kicks, 2);

	zassert_true(network_manager_process() > 0);
	/* The confirm's response was lost: the retry gets the job, not invalid_state. */
	mutate(WEB_API_POST, path, "{}", "flow-key-00000000003");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_has(job_member), "%s", body);

	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s", id);
	get(path);
	zassert_true(body_has("\"state\":\"committed\""), "%s", body);
	zassert_true(body_has("\"remaining_seconds\":null,\"reconnect_urls\":[]"), "%s", body);

	get("/api/v1/network/config");
	zassert_true(body_has("{\"revision\":1,"), "%s", body);
	zassert_true(body_has("\"address\":\"192.168.88.50\",\"prefix_length\":24,"
			      "\"gateway\":\"192.168.88.1\""),
		     "%s", body);
	zassert_true(body_has("\"pending_transaction_id\":null"), "%s", body);

	snprintf(path, sizeof(path), "/api/v1/jobs/%s", job_id);
	get(path);
	zassert_true(body_has("\"kind\":\"network_apply\""), "%s", body);
	zassert_true(body_has("\"state\":\"succeeded\""), "%s", body);
	zassert_true(body_has("\"resource_url\":\"/api/v1/network/config\""), "%s", body);
}

ZTEST(v1, test_network_confirm_waits_for_the_interfaces)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	char path[160];

	setup_device();
	net.eth.dhcp_answers = false;
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_DHCP, NET_WIFI_OFF),
		   "wait-key-00000000001");
	zassert_true(body_string("id", id, sizeof(id)));
	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s/apply", id);
	mutate(WEB_API_POST, path, "{\"confirmation_timeout_seconds\":60}", "wait-key-00000000002");
	zassert_true(network_manager_process() > 0);

	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s/confirm", id);
	mutate(WEB_API_POST, path, "{}", "wait-key-00000000003");
	zassert_equal(ctx.rsp.status, 409, "no lease yet: %s", body);
	zassert_true(body_has("\"code\":\"invalid_state\""));

	net.eth.dhcp_answers = true;
	mutate(WEB_API_POST, path, "{}", "wait-key-00000000003");
	zassert_equal(ctx.rsp.status, 202, "the corrected retry under the same key: %s", body);
}

ZTEST(v1, test_network_rollback_discards_or_restores)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	char apply_job[JOB_ID_MAX_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char path[160];

	setup_device();

	/* A staged candidate: its own short job. */
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC, NET_WIFI_OFF),
		   "back-key-00000000001");
	zassert_true(body_string("id", id, sizeof(id)));
	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s", id);
	mutate(WEB_API_DELETE, path, NULL, "back-key-00000000002");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	get(path);
	zassert_true(body_has("\"state\":\"rolled_back\""), "%s", body);
	snprintf(path, sizeof(path), "/api/v1/jobs/%s", job_id);
	get(path);
	zassert_true(body_has("\"kind\":\"network_discard\""), "%s", body);
	zassert_true(body_has("\"state\":\"succeeded\""), "%s", body);

	/* An applied one: the apply job, restored by the worker. */
	stage_and_apply(id, sizeof(id), 120, "back-key-00000000003", "back-key-00000000004");
	zassert_true(body_string("job_id", apply_job, sizeof(apply_job)));
	zassert_true(network_manager_process() > 0);
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_STATIC);

	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s", id);
	mutate(WEB_API_DELETE, path, NULL, "back-key-00000000005");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	zassert_str_equal(job_id, apply_job);
	mutate(WEB_API_DELETE, path, NULL, "back-key-00000000005");
	zassert_equal(ctx.rsp.status, 202, "a replay, not invalid_state: %s", body);

	zassert_true(network_manager_process() > 0);
	get(path);
	zassert_true(body_has("\"state\":\"rolled_back\""), "%s", body);
	zassert_true(body_has("\"error\":null"), "%s", body);
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP);
}

ZTEST(v1, test_network_confirmation_timeout_is_resource_expired)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	char path[160];

	setup_device();
	stage_and_apply(id, sizeof(id), 60, "late-key-00000000001", "late-key-00000000002");
	zassert_true(network_manager_process() > 0);
	net_now += 61 * 1000;

	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s", id);
	get(path);
	zassert_true(body_has("\"state\":\"rolling_back\""), "%s", body);
	zassert_true(body_has("\"error\":{\"code\":\"resource_expired\",\"message\":"
			      "\"Confirmation timed out; the previous configuration was restored\""),
		     "%s", body);

	zassert_true(network_manager_process() > 0);
	get(path);
	zassert_true(body_has("\"state\":\"rolled_back\""), "%s", body);
}

ZTEST(v1, test_network_unknown_and_lost_transactions)
{
	struct device_config_recovery_report report = {
		.result = DEVICE_CONFIG_RECOVERY_ROLLED_BACK,
		.transaction_id = "txn_00000009",
	};

	setup_device();
	get("/api/v1/network/transactions/txn_ffffffff");
	zassert_equal(ctx.rsp.status, 404, "%s", body);

	network_manager_boot(&report);
	get("/api/v1/network/transactions/txn_00000009");
	zassert_equal(ctx.rsp.status, 410, "%s", body);
	zassert_true(body_has("\"code\":\"boot_changed\""));
}

ZTEST(v1, test_network_apply_timeout_bounds_are_the_schemas)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	char path[160];

	setup_device();
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC, NET_WIFI_OFF),
		   "bound-key-0000000001");
	zassert_true(body_string("id", id, sizeof(id)));
	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s/apply", id);

	mutate(WEB_API_POST, path, "{\"confirmation_timeout_seconds\":59}", "bound-key-0000000002");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_true(body_has("{\"path\":\"/confirmation_timeout_seconds\","
			      "\"code\":\"out_of_range\"}"),
		     "%s", body);
	mutate(WEB_API_POST, path, "{\"confirmation_timeout_seconds\":301}", "bound-key-0000000003");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_equal(kicks, 0);
}

ZTEST(v1, test_wifi_scan_results)
{
	char job_id[JOB_ID_MAX_LEN + 1];
	char first[JOB_ID_MAX_LEN + 1];
	char path[160];
	char expected[768];
	struct job_snapshot other;
	const struct job_create_params upload = {.kind = JOB_KIND_UPLOAD_CHUNK};
	struct network_access_point *a = &net.scan_results.items[0];
	struct network_access_point *b = &net.scan_results.items[1];

	setup_device();
	net.scan_results.count = 2;
	a->ssid_len = 8;
	memcpy(a->ssid, "\xd0\x9a\xd0\xb5\xd0\xb4\xd1\x80", 8);
	memcpy(a->bssid, "\xa4\x2b\xb0\x11\x22\x33", 6);
	a->channel = 6;
	a->rssi = -41;
	a->security = NETWORK_AP_WPA3_SAE;
	b->ssid_len = 3;
	memcpy(b->ssid, "f\xff" "o", 3);
	memcpy(b->bssid, "\xde\xad\xbe\xef\x00\x03", 6);
	b->channel = 9;
	b->rssi = -63;
	b->security = NETWORK_AP_ENTERPRISE;
	b->connect_supported = true;

	mutate(WEB_API_POST, "/api/v1/network/wifi/scans", "{}", "scan-key-000000000002");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	snprintf(expected, sizeof(expected), "\"resource_url\":\"/api/v1/network/wifi/scans/%s\"",
		 job_id);
	zassert_true(body_has(expected), "%s", body);
	zassert_equal(kicks, 1);

	snprintf(path, sizeof(path), "/api/v1/network/wifi/scans/%s", job_id);
	get(path);
	snprintf(expected, sizeof(expected),
		 "{\"job_id\":\"%s\",\"state\":\"queued\",\"items\":[],\"truncated\":false,"
		 "\"error\":null}",
		 job_id);
	zassert_str_equal(body, expected);

	zassert_true(network_manager_process() > 0);
	get(path);
	snprintf(expected, sizeof(expected),
		 "{\"job_id\":\"%s\",\"state\":\"succeeded\",\"items\":["
		 "{\"ssid\":\"Кедр\",\"ssid_base64\":\"0JrQtdC00YA=\","
		 "\"bssid\":\"a4:2b:b0:11:22:33\",\"channel\":6,\"rssi_dbm\":-41,"
		 "\"security\":\"wpa3_sae\",\"connect_supported\":true},"
		 "{\"ssid\":\"f\xef\xbf\xbd" "o\",\"ssid_base64\":\"Zv9v\","
		 "\"bssid\":\"de:ad:be:ef:00:03\",\"channel\":9,\"rssi_dbm\":-63,"
		 "\"security\":\"enterprise\",\"connect_supported\":false}"
		 "],\"truncated\":false,\"error\":null}",
		 job_id);
	zassert_str_equal(body, expected);

	snprintf(path, sizeof(path), "/api/v1/jobs/%s", job_id);
	get(path);
	snprintf(expected, sizeof(expected), "\"resource_url\":\"/api/v1/network/wifi/scans/%s\"",
		 job_id);
	zassert_true(body_has(expected), "%s", body);

	/* Only the latest scan keeps its results. */
	memcpy(first, job_id, sizeof(first));
	mutate(WEB_API_POST, "/api/v1/network/wifi/scans", "{}", "scan-key-000000000003");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(network_manager_process() > 0);
	snprintf(path, sizeof(path), "/api/v1/network/wifi/scans/%s", first);
	get(path);
	zassert_equal(ctx.rsp.status, 410, "%s", body);
	zassert_true(body_has("\"code\":\"resource_expired\""));

	/* A job of another kind is not a scan. */
	zassert_equal(job_create(&upload, &other), JOB_CREATE_NEW);
	snprintf(path, sizeof(path), "/api/v1/network/wifi/scans/%s", other.id);
	get(path);
	zassert_equal(ctx.rsp.status, 404, "%s", body);
}

/*
 * ssid_base64 is decoded as strictly as the mock's b64decode(validate=True).
 * Zephyr's decoder is more lenient — it skips trailing spaces and does not
 * insist on padding — so the binding checks the alphabet and the length itself.
 * Found by mutation: "not base64!" fails on its length alone, so dropping
 * either check broke nothing.
 */
ZTEST(v1, test_network_ssid_base64_is_as_strict_as_the_mocks)
{
	static const char *const bad[] = {"Zm9v    ", "Zm9vYg"};
	char wifi[256];
	char key[32];

	setup_device();
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		snprintf(wifi, sizeof(wifi),
			 "{\"enabled\":true,\"ssid_base64\":\"%s\",\"security\":\"open\","
			 "\"hidden\":false,\"ipv4\":" NET_IPV4_DHCP ",\"credential\":{\"action\":\"clear\"}}",
			 bad[i]);
		snprintf(key, sizeof(key), "b64-key-00000000000%zu", i);
		stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC, wifi), key);
		zassert_equal(ctx.rsp.status, 422, "'%s': %s", bad[i], body);
		zassert_true(body_has("\"fields\":[{\"path\":\"/config/interfaces/wifi/ssid_base64\","
				      "\"code\":\"invalid_format\"}]"),
			     "'%s': %s", bad[i], body);
	}
}

/*
 * CredentialChange's branches both ways: a replace without a value is refused
 * on the object, like keep with one. With Wi-Fi disabled the manager's own rules
 * never look at the credential, so only the binding can refuse it. Found by
 * mutation: only the keep-with-a-value direction was tested.
 */
ZTEST(v1, test_network_a_replace_without_a_value_is_refused_as_the_object)
{
	setup_device();
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC,
				 "{\"enabled\":false,\"ssid_base64\":\"\",\"security\":\"open\","
				 "\"hidden\":false,\"ipv4\":" NET_IPV4_DHCP ",\"credential\":"
				 "{\"action\":\"replace\"}}"),
		   "stage-key-0000000021");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_true(body_has("\"fields\":[{\"path\":\"/config/interfaces/wifi/credential\","
			      "\"code\":\"conflicting\"}]"),
		     "%s", body);
}

/*
 * An SSID for display: each maximal invalid subpart becomes one U+FFFD, as
 * Python's decode(..., "replace") gives the mock — a truncated sequence is one,
 * an overlong or surrogate sequence is one per byte — and a NUL byte is replaced
 * too, the device's decision. Found by mutation: the only invalid SSID tested was
 * a lone 0xFF, which every variant of these rules renders the same.
 */
ZTEST(v1, test_wifi_scan_ssid_text_replaces_each_invalid_subpart)
{
	char job_id[JOB_ID_MAX_LEN + 1];
	char path[160];
	static const struct {
		const char *bytes;
		uint8_t len;
		const char *fragment;
	} cases[] = {
		{"\xe2\x82x", 3, "\"ssid\":\"\xef\xbf\xbdx\",\"ssid_base64\":\"4oJ4\""},
		{"\xe0\x80\x80", 3,
		 "\"ssid\":\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\",\"ssid_base64\":\"4ICA\""},
		{"\xed\xa0\x80", 3,
		 "\"ssid\":\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\",\"ssid_base64\":\"7aCA\""},
		{"a\0b", 3, "\"ssid\":\"a\xef\xbf\xbd" "b\",\"ssid_base64\":\"YQBi\""},
	};

	setup_device();
	net.scan_results.count = ARRAY_SIZE(cases);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		net.scan_results.items[i].ssid_len = cases[i].len;
		memcpy(net.scan_results.items[i].ssid, cases[i].bytes, cases[i].len);
		net.scan_results.items[i].channel = 6;
		net.scan_results.items[i].security = NETWORK_AP_OPEN;
	}
	mutate(WEB_API_POST, "/api/v1/network/wifi/scans", "{}", "scan-key-000000000011");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	zassert_true(network_manager_process() > 0);
	snprintf(path, sizeof(path), "/api/v1/network/wifi/scans/%s", job_id);
	get(path);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		zassert_true(body_has(cases[i].fragment), "case %zu: %s", i, body);
	}
}

/*
 * An adapter's channel and RSSI are brought into the schema's ranges rather than
 * passed through: a response outside its own schema is one a strict client
 * rejects whole. Found by mutation: every reading tested was already in range.
 */
ZTEST(v1, test_wifi_scan_channel_and_rssi_stay_in_the_schemas_ranges)
{
	char job_id[JOB_ID_MAX_LEN + 1];
	char path[160];

	setup_device();
	net.scan_results.count = 2;
	net.scan_results.items[0].ssid_len = 1;
	net.scan_results.items[0].ssid[0] = 'a';
	net.scan_results.items[0].channel = 250;
	net.scan_results.items[0].rssi = 20;
	net.scan_results.items[1].ssid_len = 1;
	net.scan_results.items[1].ssid[0] = 'b';
	net.scan_results.items[1].channel = 0;
	net.scan_results.items[1].rssi = -128;
	mutate(WEB_API_POST, "/api/v1/network/wifi/scans", "{}", "scan-key-000000000012");
	zassert_true(body_string("job_id", job_id, sizeof(job_id)), "%s", body);
	zassert_true(network_manager_process() > 0);
	snprintf(path, sizeof(path), "/api/v1/network/wifi/scans/%s", job_id);
	get(path);
	zassert_true(body_has("\"ssid\":\"a\",\"ssid_base64\":\"YQ==\","
			      "\"bssid\":\"00:00:00:00:00:00\",\"channel\":233,\"rssi_dbm\":0"),
		     "%s", body);
	zassert_true(body_has("\"ssid\":\"b\",\"ssid_base64\":\"Yg==\","
			      "\"bssid\":\"00:00:00:00:00:00\",\"channel\":1,\"rssi_dbm\":-127"),
		     "%s", body);
}

/*
 * RFC 5952: a single zero group is written out, and of two equally long runs
 * the first becomes "::". Found by mutation: the one address tested had a
 * single run of three, which either variant compresses the same way.
 */
ZTEST(v1, test_network_status_writes_ipv6_as_rfc5952)
{
	setup_device();
	net.eth.extra_count = 1;
	net.eth.extra[0] = (struct network_addr){
		.family = DEVICE_CONFIG_AF_INET6,
		.prefix_length = 64,
		.source = NETWORK_ADDR_SLAAC,
		.bytes = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
	};
	network_manager_boot(NULL);
	zassert_true(network_manager_process() > 0);

	get("/api/v1/network/status");
	zassert_true(body_has("\"address\":\"2001:db8:0:1:1:1:1:1\""), "one zero group stays: %s",
		     body);

	memcpy(net.eth.extra[0].bytes,
	       (uint8_t[16]){0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1}, 16);
	get("/api/v1/network/status");
	zassert_true(body_has("\"address\":\"2001:db8::1:0:0:1\""),
		     "the first of two equal runs is compressed: %s", body);
}

/*
 * The associated network's RSSI is brought into the schema's range like a scan
 * result's: a driver's reading above 0 or below -127 is clamped, not passed on.
 * Found by mutation: the fake only ever reported -55.
 */
ZTEST(v1, test_network_status_rssi_stays_in_the_schemas_range)
{
	setup_device();
	net.associated = true;

	net.rssi = 20;
	get("/api/v1/network/status");
	zassert_equal(ctx.rsp.status, 200, "%s", body);
	zassert_true(body_has("\"ssid\":\"\",\"rssi_dbm\":0,"), "%s", body);

	net.rssi = -128;
	get("/api/v1/network/status");
	zassert_true(body_has("\"ssid\":\"\",\"rssi_dbm\":-127,"), "%s", body);
}

/*
 * reconnect_urls name where the device will answer while a change awaits its
 * confirmation: the static address of an enabled interface, not one kept on a
 * disabled interface as a stored profile. Found by mutation: the URLs were only
 * read while applying, and the disabled interface in every case was on DHCP.
 */
ZTEST(v1, test_network_reconnect_urls_while_awaiting_name_only_enabled_interfaces)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	char path[160];

	setup_device();
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC,
				 "{\"enabled\":false,\"ssid_base64\":\"\",\"security\":\"open\","
				 "\"hidden\":false,\"ipv4\":{\"mode\":\"static\","
				 "\"address\":\"192.168.88.60\",\"prefix_length\":24,\"gateway\":null},"
				 "\"credential\":{\"action\":\"keep\"}}"),
		   "urls-key-00000000001");
	zassert_equal(ctx.rsp.status, 201, "%s", body);
	zassert_true(body_string("id", id, sizeof(id)));
	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s/apply", id);
	mutate(WEB_API_POST, path, "{\"confirmation_timeout_seconds\":120}", "urls-key-00000000002");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(network_manager_process() > 0);

	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s", id);
	get(path);
	zassert_true(body_has("\"state\":\"awaiting_confirmation\""), "%s", body);
	zassert_true(body_has("\"reconnect_urls\":[\"http://192.168.88.50/\"]"), "%s", body);
}

/*
 * The replay table is full at eight records, and the one that gives way is the
 * oldest: the request a client made most recently is the one whose response it
 * is most likely still waiting for. Found by mutation: evicting the newest broke
 * nothing, because no test filled the table.
 */
ZTEST(v1, test_network_the_oldest_replay_record_gives_way)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	char again[NETWORK_TXN_ID_MAX_LEN + 1];
	char key[32];
	char path[160];
	char json[sizeof(net_json)];

	setup_device();
	strcpy(json, net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC, NET_WIFI_OFF));
	/* Ten records, each later than the last: the table is full well before the end. */
	for (int i = 0; i < 5; i++) {
		snprintf(key, sizeof(key), "evict-stage-00000%03d", i);
		stage_body(json, key);
		zassert_equal(ctx.rsp.status, 201, "%s", body);
		zassert_true(body_string("id", id, sizeof(id)));
		k_sleep(K_MSEC(2));
		snprintf(key, sizeof(key), "evict-discard-000%03d", i);
		snprintf(path, sizeof(path), "/api/v1/network/transactions/%s", id);
		mutate(WEB_API_DELETE, path, NULL, key);
		zassert_equal(ctx.rsp.status, 202, "%s", body);
		k_sleep(K_MSEC(2));
	}

	/* The last stage is the second newest record, so it is still there. */
	stage_body(json, "evict-stage-00000004");
	zassert_equal(ctx.rsp.status, 201, "%s", body);
	zassert_true(body_string("id", again, sizeof(again)));
	zassert_str_equal(again, id, "the retry of the latest stage gets its transaction back");
}

/*
 * A replay record lasts fifteen minutes. Past that the same key and body are a
 * new request, which here meets the candidate the first one staged. Found by
 * mutation: records that never expired broke nothing, because no test waited.
 */
ZTEST(v1, test_network_a_replay_record_expires)
{
	setup_device();
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC, NET_WIFI_OFF),
		   "expire-key-000000001");
	zassert_equal(ctx.rsp.status, 201, "%s", body);

	k_sleep(K_MINUTES(16));
	stage_body(net_candidate(0, NET_DNS_AUTO, NET_ETH_STATIC, NET_WIFI_OFF),
		   "expire-key-000000001");
	zassert_equal(ctx.rsp.status, 409, "past fifteen minutes it is not a replay: %s", body);
	zassert_true(body_has("\"code\":\"busy\""), "%s", body);
}

/*
 * The worker is woken for what was accepted and for nothing else: a refused
 * apply or confirm changed nothing it would have to act on. Found by mutation:
 * kicking on a refusal broke nothing, because only a refused scan counted kicks.
 */
ZTEST(v1, test_network_a_refused_apply_or_confirm_wakes_nobody)
{
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	char path[160];

	setup_device();
	mutate(WEB_API_POST, "/api/v1/network/transactions/txn_ffffffff/apply",
	       "{\"confirmation_timeout_seconds\":60}", "kick-key-00000000001");
	zassert_equal(ctx.rsp.status, 404, "%s", body);
	zassert_equal(kicks, 0, "a refused apply wakes nobody");

	stage_and_apply(id, sizeof(id), 60, "kick-key-00000000002", "kick-key-00000000003");
	zassert_equal(kicks, 1);

	/* Not applied yet: confirm is refused. */
	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s/confirm", id);
	mutate(WEB_API_POST, path, "{}", "kick-key-00000000004");
	zassert_equal(ctx.rsp.status, 409, "%s", body);
	zassert_equal(kicks, 1, "a refused confirm wakes nobody");
}

/* -- Matter ---------------------------------------------------------------- */

ZTEST(v1, test_matter_before_the_stack_starts)
{
	char job_id[JOB_ID_MAX_LEN + 1];

	setup_device();

	get("/api/v1/matter/status");
	zassert_equal(ctx.rsp.status, 200);
	zassert_str_equal(body, "{\"state\":\"not_ready\",\"commissioned\":false,\"fabric_count\":0,"
				"\"error\":null}");
	get("/api/v1/matter/commissioning");
	zassert_str_equal(body, "{\"open\":false,\"mode\":null,\"source\":null,"
				"\"remaining_seconds\":0,\"codes_available\":false}");
	get("/api/v1/matter/onboarding-codes");
	zassert_str_equal(body, "{\"available\":false,\"reason\":\"service_not_ready\","
				"\"qr_payload\":null,\"manual_pairing_code\":null,"
				"\"setup_passcode\":null}");
	get("/api/v1/matter/fabrics");
	zassert_str_equal(body, "{\"items\":[],\"count\":0}");

	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", "matter-open-00000001");
	zassert_equal(ctx.rsp.status, 503, "%s", body);
	zassert_true(body_has("\"code\":\"service_not_ready\""), "%s", body);
	zassert_true(body_has("\"retryable\":true"), "%s", body);
	matter_close_request("matter-close-0000001");
	zassert_equal(ctx.rsp.status, 503, "%s", body);
	zassert_equal(mf.queued, 0, "nothing reaches the stack");

	/* Once the stack runs, the same keys are new requests: the refusals left
	 * no job behind to answer them with. */
	matter_ready();
	matter_close_request("matter-close-0000001");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	get_job(job_id);
	zassert_true(body_has("\"state\":\"running\""), "%s", body);
	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", "matter-open-00000001");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	get_job(job_id);
	zassert_true(body_has("\"state\":\"running\""), "%s", body);
}

ZTEST(v1, test_matter_opening_a_window_is_a_job_the_matter_thread_finishes)
{
	char job_id[JOB_ID_MAX_LEN + 1];
	char location[80];

	setup_device();
	matter_ready();
	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", "matter-open-00000001");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_has("\"resource_url\":\"/api/v1/matter/commissioning\""), "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	snprintf(location, sizeof(location), "/api/v1/jobs/%s", job_id);
	zassert_str_equal(response_header("Location"), location);

	get_job(job_id);
	zassert_true(body_has("\"kind\":\"matter_open\""), "%s", body);
	zassert_true(body_has("\"state\":\"running\""), "%s", body);
	zassert_true(body_has("\"phase\":\"opening\""), "%s", body);
	get("/api/v1/matter/commissioning");
	zassert_true(body_has("\"open\":false"), "not open before the Matter thread ran: %s", body);

	matter_drain();
	get_job(job_id);
	zassert_true(body_has("\"state\":\"succeeded\""), "%s", body);
	zassert_true(body_has("\"resource_url\":\"/api/v1/matter/commissioning\""), "%s", body);

	get("/api/v1/matter/commissioning");
	zassert_str_equal(body, "{\"open\":true,\"mode\":\"basic\",\"source\":\"web\","
				"\"remaining_seconds\":300,\"codes_available\":true}");
	get("/api/v1/matter/onboarding-codes");
	zassert_str_equal(body, "{\"available\":true,\"reason\":null,"
				"\"qr_payload\":\"MT:Y.K9042C00KA0648G00\","
				"\"manual_pairing_code\":\"01234567890\","
				"\"setup_passcode\":\"00012345\"}");
}

ZTEST(v1, test_matter_a_refused_open_leaves_nothing_under_its_key)
{
	char job_id[JOB_ID_MAX_LEN + 1];

	setup_device();
	matter_ready();
	mf.window = (struct matter_window_reading){.open = true};
	matter_service_report_window_changed();

	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", "matter-open-00000001");
	zassert_equal(ctx.rsp.status, 409, "%s", body);
	zassert_true(body_has("\"code\":\"invalid_state\""), "%s", body);

	mf.window = (struct matter_window_reading){0};
	matter_service_report_window_changed();
	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", "matter-open-00000001");
	zassert_equal(ctx.rsp.status, 202, "the corrected retry is not answered with the refusal: %s",
		      body);
	/* A new job, not a failed one the refusal left under the key. */
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	get_job(job_id);
	zassert_true(body_has("\"state\":\"running\""), "%s", body);
}

ZTEST(v1, test_matter_a_retry_of_an_accepted_open_gets_the_same_job)
{
	char first[JOB_ID_MAX_LEN + 1];
	char second[JOB_ID_MAX_LEN + 1];

	setup_device();
	matter_ready();
	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", "matter-open-00000001");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", first, sizeof(first)));
	matter_drain();

	/* The window is open now; the retry is still the same request. */
	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", "matter-open-00000001");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", second, sizeof(second)));
	zassert_str_equal(first, second);
	zassert_equal(mf.queued, 0, "and nothing is asked of the stack again");

	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":600}", "matter-open-00000001");
	zassert_true(body_has("\"code\":\"idempotency_conflict\""), "%s", body);

	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", "matter-open-00000002");
	zassert_equal(ctx.rsp.status, 409, "a new request finds the window open: %s", body);
}

ZTEST(v1, test_matter_window_request_validation)
{
	setup_device();
	matter_ready();

	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":179}", "matter-check-0000001");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_true(body_has("\"fields\":[{\"path\":\"/timeout_seconds\",\"code\":\"out_of_range\"}]"),
		     "%s", body);
	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":901}", "matter-check-0000002");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	matter_open_request("{\"mode\":\"enhanced\",\"timeout_seconds\":300}", "matter-check-0000003");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_true(body_has("\"fields\":[{\"path\":\"/mode\",\"code\":\"not_allowed\"}]"), "%s", body);
	matter_open_request("{\"timeout_seconds\":300}", "matter-check-0000004");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", NULL);
	zassert_equal(ctx.rsp.status, 422, "the Idempotency-Key is required: %s", body);
	zassert_equal(mf.queued, 0);
}

ZTEST(v1, test_matter_narrower_sdk_limits_are_published_and_enforced)
{
	setup_device();
	mf.min_s = 300;
	mf.max_s = 600;
	matter_ready();

	get("/api/v1/capabilities");
	zassert_true(body_has("\"matter\":{\"available\":true,\"reason\":null}"), "%s", body);
	zassert_true(body_has("\"commissioning_min_seconds\":300,\"commissioning_max_seconds\":600"),
		     "%s", body);
	/* Inside the document's range, outside the device's. */
	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":250}", "matter-check-0000001");
	zassert_equal(ctx.rsp.status, 422, "%s", body);
	zassert_true(body_has("\"fields\":[{\"path\":\"/timeout_seconds\",\"code\":\"out_of_range\"}]"),
		     "%s", body);
}

ZTEST(v1, test_matter_a_stack_refusal_fails_the_job)
{
	char job_id[JOB_ID_MAX_LEN + 1];

	setup_device();
	matter_ready();
	mf.open_result = -EBUSY;
	matter_open_request("{\"mode\":\"basic\",\"timeout_seconds\":300}", "matter-open-00000001");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	matter_drain();
	get_job(job_id);
	zassert_true(body_has("\"state\":\"failed\""), "%s", body);
	zassert_true(body_has("\"code\":\"invalid_state\""), "%s", body);
}

ZTEST(v1, test_matter_closing_a_closed_window_succeeds)
{
	char job_id[JOB_ID_MAX_LEN + 1];

	setup_device();
	matter_ready();
	matter_close_request("matter-close-0000001");
	zassert_equal(ctx.rsp.status, 202, "%s", body);
	zassert_true(body_string("job_id", job_id, sizeof(job_id)));
	get_job(job_id);
	zassert_true(body_has("\"kind\":\"matter_close\""), "%s", body);
	zassert_true(body_has("\"phase\":\"closing\""), "%s", body);
	matter_drain();
	get_job(job_id);
	zassert_true(body_has("\"state\":\"succeeded\""), "%s", body);
}

ZTEST(v1, test_matter_a_failed_stack_reports_why)
{
	char request_id[65];

	setup_device();
	matter_service_report_starting();
	matter_service_report_started(-5);
	get("/api/v1/matter/status");
	zassert_equal(ctx.rsp.status, 200);
	zassert_true(body_has("{\"state\":\"failed\",\"commissioned\":false,\"fabric_count\":0,"
			      "\"error\":{\"code\":\"service_not_ready\",\"message\":\"The Matter stack "
			      "failed to start (0xfffffffb)\",\"request_id\":\""),
		     "%s", body);
	snprintf(request_id, sizeof(request_id), "\"request_id\":\"%s\"",
		 response_header("X-Request-ID"));
	zassert_true(body_has(request_id), "the error carries this request's id: %s", body);
	get("/api/v1/capabilities");
	zassert_true(body_has("\"matter\":{\"available\":false,\"reason\":\"failed\"}"), "%s", body);
}

ZTEST(v1, test_matter_fabrics_are_hex_and_labels_are_escaped)
{
	setup_device();
	mf.count = 1;
	strcpy(mf.table[0].id, "00112233AABBCCDD:FAB0000000000001");
	mf.table[0].fabric_index = 1;
	mf.table[0].fabric_id = 0xFAB0000000000001ULL;
	mf.table[0].node_id = 0x1B669ULL;
	mf.table[0].vendor_id = 0xFFF1;
	strcpy(mf.table[0].label, "say \"hi\"");
	matter_ready();

	get("/api/v1/matter/fabrics");
	zassert_str_equal(body, "{\"items\":[{\"id\":\"00112233AABBCCDD:FAB0000000000001\","
				"\"fabric_index\":1,\"fabric_id\":\"FAB0000000000001\","
				"\"node_id\":\"000000000001B669\",\"vendor_id\":65521,"
				"\"label\":\"say \\\"hi\\\"\"}],\"count\":1}");
	get("/api/v1/matter/status");
	zassert_str_equal(body, "{\"state\":\"ready\",\"commissioned\":true,\"fabric_count\":1,"
				"\"error\":null}");
}
