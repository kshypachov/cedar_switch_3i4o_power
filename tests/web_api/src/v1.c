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

static void *suite_setup(void)
{
	zassert_ok(web_api_v1_init(&identity));
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	harness_reset();
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
		"\"matter\":{\"available\":false,\"reason\":\"not_implemented\"},"
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
