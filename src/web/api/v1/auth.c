/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1: setup, sessions and the password change.
 *
 * Contract: "Сессия и устройство" in api-contract.md. The rules live in
 * web-auth and web-api; this file turns their results into the schemas.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "v1_internal.h"

LOG_MODULE_REGISTER(web_api_v1_auth, LOG_LEVEL_INF);

/* -- request schemas ---------------------------------------------------- */

static const struct web_json_field setup_fields[] = {
	{
		.name = "password",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_setup_body, password),
		.size = sizeof(((struct v1_setup_body *)0)->password),
		.min_len = WEB_AUTH_PASSWORD_MIN_CHARS,
		.max_len = WEB_AUTH_PASSWORD_MAX_CHARS,
	},
};

const struct web_json_object v1_setup_schema = {
	.fields = setup_fields,
	.field_count = ARRAY_SIZE(setup_fields),
};

static const struct web_json_field login_fields[] = {
	{
		.name = "password",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_login_body, password),
		.size = sizeof(((struct v1_login_body *)0)->password),
		.min_len = 1,
		.max_len = WEB_AUTH_PASSWORD_MAX_CHARS,
	},
};

const struct web_json_object v1_login_schema = {
	.fields = login_fields,
	.field_count = ARRAY_SIZE(login_fields),
};

static const struct web_json_field password_fields[] = {
	{
		.name = "current_password",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_password_body, current_password),
		.size = sizeof(((struct v1_password_body *)0)->current_password),
		.min_len = 1,
		.max_len = WEB_AUTH_PASSWORD_MAX_CHARS,
	},
	{
		.name = "new_password",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_password_body, new_password),
		.size = sizeof(((struct v1_password_body *)0)->new_password),
		.min_len = WEB_AUTH_PASSWORD_MIN_CHARS,
		.max_len = WEB_AUTH_PASSWORD_MAX_CHARS,
	},
};

const struct web_json_object v1_password_schema = {
	.fields = password_fields,
	.field_count = ARRAY_SIZE(password_fields),
};

/* -- responses ---------------------------------------------------------- */

#define SESSION_URL WEB_API_BASE_PATH "/auth/session"

static void write_session(struct web_api_call *call, const struct web_auth_session *s)
{
	struct web_json_writer *w = web_api_json(call);

	web_json_object_begin(w);
	web_json_key(w, "username");
	web_json_string(w, "admin");
	web_json_key(w, "csrf_token");
	web_json_string(w, s->csrf_token);
	web_json_key(w, "idle_timeout_seconds");
	web_json_int(w, s->idle_timeout_seconds);
	web_json_key(w, "absolute_remaining_seconds");
	web_json_int(w, s->absolute_remaining_seconds);
	web_json_object_end(w);
}

void v1_get_auth_state(struct web_api_call *call)
{
	struct web_auth_state state;
	struct web_json_writer *w = web_api_json(call);

	web_auth_get_state(&state);
	web_json_object_begin(w);
	web_json_key(w, "setup_required");
	web_json_bool(w, state.setup_required);
	web_json_key(w, "setup_allowed");
	web_json_bool(w, state.setup_allowed);
	web_json_key(w, "setup_token");
	web_json_string_or_null(w, state.setup_allowed ? state.setup_token : NULL);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

void v1_setup_admin(struct web_api_call *call)
{
	const struct v1_setup_body *body = call->body;
	struct web_auth_session session;
	struct web_auth_result r = web_auth_setup(call->req->headers.setup_token, body->password,
						  strlen(body->password), &call->req->peer,
						  &session);

	if (r.status != WEB_AUTH_OK) {
		web_api_reject_auth(call, &r);
		return;
	}
	web_api_set_session_cookie(call, session.token, session.absolute_remaining_seconds);
	web_api_set_location(call, SESSION_URL);
	write_session(call, &session);
	web_api_reply_json(call, 201);
	memset(&session, 0, sizeof(session));
	LOG_INF("administrator created");
}

void v1_login(struct web_api_call *call)
{
	const struct v1_login_body *body = call->body;
	struct web_auth_session session;
	struct web_auth_result r = web_auth_login(body->password, strlen(body->password),
						  &call->req->peer, &session);

	if (r.status != WEB_AUTH_OK) {
		web_api_reject_auth(call, &r);
		return;
	}
	web_api_set_session_cookie(call, session.token, session.absolute_remaining_seconds);
	write_session(call, &session);
	web_api_reply_json(call, 200);
	memset(&session, 0, sizeof(session));
}

void v1_get_session(struct web_api_call *call)
{
	write_session(call, &call->session);
	web_api_reply_json(call, 200);
}

void v1_logout(struct web_api_call *call)
{
	(void)web_auth_logout(call->session.token);
	web_api_expire_session_cookie(call);
	web_api_reply_empty(call, 204);
}

/* -- password change ----------------------------------------------------- */

/*
 * One worker thread for jobs that must not run on the HTTP server's thread. A
 * password change derives a key and writes flash; nothing else here needs a
 * worker yet. web-auth allows one pending change at a time, so one work item
 * and one job id suffice.
 */
#define WORKER_STACK_SIZE 4096
#define WORKER_PRIORITY   K_PRIO_PREEMPT(10)

K_THREAD_STACK_DEFINE(v1_worker_stack, WORKER_STACK_SIZE);
static struct k_work_q v1_worker;
static struct k_work password_work;
static char password_job[JOB_ID_MAX_LEN + 1];
static bool worker_started;

static void run_password_change(struct k_work *work)
{
	enum web_auth_status status;

	ARG_UNUSED(work);
	(void)job_set_state(password_job, JOB_STATE_RUNNING);
	(void)job_set_phase(password_job, "hashing");

	status = web_auth_password_change_run();
	if (status == WEB_AUTH_OK) {
		(void)job_set_state(password_job, JOB_STATE_SUCCEEDED);
		LOG_INF("administrator password changed; all sessions ended");
	} else {
		LOG_ERR("password change failed: %s", web_auth_status_str(status));
		(void)job_fail(password_job, "internal_error", false);
	}
}

int v1_auth_worker_start(void)
{
	if (worker_started) {
		return 0;
	}
	k_work_queue_init(&v1_worker);
	k_work_queue_start(&v1_worker, v1_worker_stack, K_THREAD_STACK_SIZEOF(v1_worker_stack),
			   WORKER_PRIORITY, &(struct k_work_queue_config){.name = "web_v1_jobs"});
	k_work_init(&password_work, run_password_change);
	worker_started = true;

	return 0;
}

void v1_change_password(struct web_api_call *call)
{
	const struct v1_password_body *body = call->body;
	struct job_snapshot job;
	struct web_auth_result r;
	const struct job_create_params params = {
		.kind = JOB_KIND_PASSWORD_CHANGE,
		.cancellable = false,
		.idempotency_key = call->scoped_key,
		.request_hash = call->request_hash,
	};

	/* A retry of a change already accepted gets the same job, without a
	 * second derivation and without re-checking a password it has already
	 * proved. */
	switch (job_find_by_key(call->scoped_key, call->request_hash, &job)) {
	case JOB_LOOKUP_EXISTING:
		web_api_reply_accepted(call, job.id, SESSION_URL);
		return;
	case JOB_LOOKUP_CONFLICT:
		web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
			       "This Idempotency-Key was used with a different request");
		return;
	default:
		break;
	}

	r = web_auth_password_change_begin(body->current_password, strlen(body->current_password),
					   body->new_password, strlen(body->new_password),
					   &call->req->peer);
	if (r.status != WEB_AUTH_OK) {
		web_api_reject_auth(call, &r);
		return;
	}

	switch (job_create(&params, &job)) {
	case JOB_CREATE_NEW:
		break;
	case JOB_CREATE_EXHAUSTED:
		web_auth_password_change_abandon();
		(void)api_error_init(web_api_error(call), API_ERR_RATE_LIMITED,
				     "Too many operations are in flight; retry shortly", NULL);
		(void)api_error_set_retry_after(web_api_error(call), 1);
		web_api_reject_error(call);
		return;
	default:
		web_auth_password_change_abandon();
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The job could not be created");
		return;
	}

	memcpy(password_job, job.id, sizeof(password_job));
	if (!worker_started || k_work_submit_to_queue(&v1_worker, &password_work) < 0) {
		web_auth_password_change_abandon();
		(void)job_fail(job.id, "internal_error", false);
	}
	web_api_reply_accepted(call, job.id, SESSION_URL);
}
