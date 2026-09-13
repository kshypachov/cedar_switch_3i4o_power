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
#include <matter_service/matter_service.h>

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
		"\"network_confirm_min_seconds\":30,\"network_confirm_max_seconds\":900},"
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
