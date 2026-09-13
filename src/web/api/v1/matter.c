/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1: Matter status, the commissioning window, onboarding codes, fabrics.
 *
 * Contract: "Matter" in api-contract.md. The rules live in matter-service; this
 * file turns its snapshot and its request results into the schemas, and a
 * request into a job.
 *
 * A window request is checked before its job is created, so a refused request
 * (409, 503, 422) leaves nothing under its Idempotency-Key and a corrected
 * retry is not answered with the refusal. The service is asked only after the
 * job exists, because its callback needs the job id; the checks it repeats can
 * only disagree if the Matter thread changed the window in between, and then
 * the job fails with the same code the request would have got.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <matter_service/matter_service.h>

#include "v1_internal.h"

LOG_MODULE_REGISTER(web_api_v1_matter, LOG_LEVEL_INF);

#define COMMISSIONING_URL WEB_API_BASE_PATH "/matter/commissioning"

/* -- request schema -------------------------------------------------------- */

static const char *const window_modes[] = {"basic", NULL};

static const struct web_json_field commissioning_fields[] = {
	{
		.name = "mode",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_commissioning_body, mode),
		.size = sizeof(((struct v1_commissioning_body *)0)->mode),
		.min_len = 1,
		.max_len = sizeof(((struct v1_commissioning_body *)0)->mode) - 1,
		.enum_values = window_modes,
	},
	{
		.name = "timeout_seconds",
		.type = WEB_JSON_INT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_commissioning_body, timeout_seconds),
		.min = MATTER_WINDOW_MIN_SECONDS,
		.max = MATTER_WINDOW_MAX_SECONDS,
	},
};

const struct web_json_object v1_commissioning_schema = {
	.fields = commissioning_fields,
	.field_count = ARRAY_SIZE(commissioning_fields),
};

/* -- jobs the Matter thread finishes ---------------------------------------- */

struct window_job {
	bool used;
	char id[JOB_ID_MAX_LEN + 1];
};

static struct window_job window_jobs[CONFIG_MATTER_SERVICE_REQUEST_SLOTS];
static struct k_spinlock window_jobs_lock;

static struct window_job *take_window_job(const char *id)
{
	struct window_job *slot = NULL;
	k_spinlock_key_t key = k_spin_lock(&window_jobs_lock);

	for (size_t i = 0; i < ARRAY_SIZE(window_jobs); i++) {
		if (!window_jobs[i].used) {
			slot = &window_jobs[i];
			slot->used = true;
			strncpy(slot->id, id, sizeof(slot->id) - 1);
			slot->id[sizeof(slot->id) - 1] = '\0';
			break;
		}
	}
	k_spin_unlock(&window_jobs_lock, key);

	return slot;
}

static void release_window_job(struct window_job *slot)
{
	k_spinlock_key_t key = k_spin_lock(&window_jobs_lock);

	slot->used = false;
	k_spin_unlock(&window_jobs_lock, key);
}

/* On the Matter thread. */
static void window_job_done(void *ctx, int result)
{
	struct window_job *slot = ctx;
	char id[JOB_ID_MAX_LEN + 1];

	memcpy(id, slot->id, sizeof(id));
	release_window_job(slot);

	if (result == 0) {
		(void)job_set_state(id, JOB_STATE_SUCCEEDED);
	} else if (result == -EBUSY) {
		/* Another window, or a commissioning in progress, stood in the way. */
		(void)job_fail(id, "invalid_state", false);
	} else {
		LOG_ERR("commissioning window request failed: %d", result);
		(void)job_fail(id, "internal_error", false);
	}
}

/* -- helpers ------------------------------------------------------------------ */

static void reject_not_ready(struct web_api_call *call)
{
	web_api_reject(call, API_ERR_SERVICE_NOT_READY,
		       "Matter is not ready; commissioning is unavailable");
}

static void reject_window_open(struct web_api_call *call)
{
	web_api_reject(call, API_ERR_INVALID_STATE,
		       "A commissioning window is already open; close it before opening another");
}

static void reject_busy(struct web_api_call *call)
{
	(void)api_error_init(web_api_error(call), API_ERR_RATE_LIMITED,
			     "Too many operations are in flight; retry shortly", NULL);
	(void)api_error_set_retry_after(web_api_error(call), 1);
	web_api_reject_error(call);
}

static void reject_timeout(struct web_api_call *call)
{
	(void)api_error_init(web_api_error(call), API_ERR_VALIDATION_FAILED,
			     "The timeout is outside the range the device allows", NULL);
	(void)api_error_add_field(web_api_error(call), "/timeout_seconds", API_FIELD_OUT_OF_RANGE);
	web_api_reject_error(call);
}

/* The retry of an accepted request, a conflicting reuse of its key, or neither. */
static bool answered_by_key(struct web_api_call *call)
{
	struct job_snapshot job;

	switch (job_find_by_key(call->scoped_key, call->request_hash, &job)) {
	case JOB_LOOKUP_EXISTING:
		web_api_reply_accepted(call, job.id, COMMISSIONING_URL);
		return true;
	case JOB_LOOKUP_CONFLICT:
		web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
			       "This Idempotency-Key was used with a different request");
		return true;
	default:
		return false;
	}
}

static bool create_window_job(struct web_api_call *call, enum job_kind kind, const char *phase,
			      struct job_snapshot *job)
{
	const struct job_create_params params = {
		.kind = kind,
		.cancellable = false,
		.idempotency_key = call->scoped_key,
		.request_hash = call->request_hash,
	};

	switch (job_create(&params, job)) {
	case JOB_CREATE_NEW:
		(void)job_set_state(job->id, JOB_STATE_RUNNING);
		(void)job_set_phase(job->id, phase);
		return true;
	case JOB_CREATE_EXHAUSTED:
		reject_busy(call);
		return false;
	default:
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The job could not be created");
		return false;
	}
}

/* A refusal that arrived after the job was created fails the job the same way. */
static void fail_created_job(struct web_api_call *call, const char *id,
			     enum matter_request_result result)
{
	switch (result) {
	case MATTER_REQUEST_WINDOW_OPEN:
		(void)job_fail(id, "invalid_state", false);
		reject_window_open(call);
		break;
	case MATTER_REQUEST_BUSY:
		(void)job_fail(id, "rate_limited", true);
		reject_busy(call);
		break;
	case MATTER_REQUEST_OUT_OF_RANGE:
		(void)job_fail(id, "validation_failed", false);
		reject_timeout(call);
		break;
	default:
		(void)job_fail(id, "service_not_ready", true);
		reject_not_ready(call);
		break;
	}
}

/* -- handlers ------------------------------------------------------------------- */

void v1_get_matter_status(struct web_api_call *call)
{
	struct matter_status status;
	struct web_json_writer *w = web_api_json(call);

	matter_service_get_status(&status);

	web_json_object_begin(w);
	web_json_key(w, "state");
	web_json_string(w, matter_state_str(status.state));
	web_json_key(w, "commissioned");
	web_json_bool(w, status.commissioned);
	web_json_key(w, "fabric_count");
	web_json_int(w, status.fabric_count);
	web_json_key(w, "error");
	if (status.state == MATTER_STATE_FAILED) {
		char message[64];

		snprintf(message, sizeof(message), "The Matter stack failed to start (0x%08x)",
			 (unsigned int)status.failure);
		web_json_object_begin(w);
		web_json_key(w, "code");
		web_json_string(w, "service_not_ready");
		web_json_key(w, "message");
		web_json_string(w, message);
		web_json_key(w, "request_id");
		web_json_string(w, call->ctx->rsp.request_id);
		web_json_key(w, "retryable");
		web_json_bool(w, api_error_is_retryable(API_ERR_SERVICE_NOT_READY));
		web_json_object_end(w);
	} else {
		web_json_null(w);
	}
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

void v1_get_commissioning_window(struct web_api_call *call)
{
	struct matter_window window;
	struct web_json_writer *w = web_api_json(call);

	matter_service_get_window(&window);

	web_json_object_begin(w);
	web_json_key(w, "open");
	web_json_bool(w, window.open);
	web_json_key(w, "mode");
	web_json_string_or_null(w, matter_window_mode_str(window.mode));
	web_json_key(w, "source");
	web_json_string_or_null(w, matter_window_source_str(window.source));
	web_json_key(w, "remaining_seconds");
	web_json_int(w, MIN(window.remaining_seconds, MATTER_WINDOW_MAX_SECONDS));
	web_json_key(w, "codes_available");
	web_json_bool(w, window.codes_available);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

void v1_open_commissioning_window(struct web_api_call *call)
{
	const struct v1_commissioning_body *body = call->body;
	uint32_t timeout = (uint32_t)body->timeout_seconds;
	struct matter_status status;
	struct matter_window window;
	struct job_snapshot job;
	struct window_job *slot;
	uint32_t min_s;
	uint32_t max_s;
	enum matter_request_result result;

	if (answered_by_key(call)) {
		return;
	}

	matter_service_window_limits(&min_s, &max_s);
	if (timeout < min_s || timeout > max_s) {
		reject_timeout(call);
		return;
	}
	matter_service_get_status(&status);
	if (status.state != MATTER_STATE_READY) {
		reject_not_ready(call);
		return;
	}
	matter_service_get_window(&window);
	if (window.open) {
		reject_window_open(call);
		return;
	}

	if (!create_window_job(call, JOB_KIND_MATTER_OPEN, "opening", &job)) {
		return;
	}
	slot = take_window_job(job.id);
	if (slot == NULL) {
		fail_created_job(call, job.id, MATTER_REQUEST_BUSY);
		return;
	}
	result = matter_service_open_window(timeout, window_job_done, slot);
	if (result != MATTER_REQUEST_ACCEPTED) {
		release_window_job(slot);
		fail_created_job(call, job.id, result);
		return;
	}
	web_api_reply_accepted(call, job.id, COMMISSIONING_URL);
}

void v1_close_commissioning_window(struct web_api_call *call)
{
	struct matter_status status;
	struct job_snapshot job;
	struct window_job *slot;
	enum matter_request_result result;

	if (answered_by_key(call)) {
		return;
	}

	matter_service_get_status(&status);
	if (status.state != MATTER_STATE_READY) {
		reject_not_ready(call);
		return;
	}

	if (!create_window_job(call, JOB_KIND_MATTER_CLOSE, "closing", &job)) {
		return;
	}
	slot = take_window_job(job.id);
	if (slot == NULL) {
		fail_created_job(call, job.id, MATTER_REQUEST_BUSY);
		return;
	}
	result = matter_service_close_window(window_job_done, slot);
	if (result != MATTER_REQUEST_ACCEPTED) {
		release_window_job(slot);
		fail_created_job(call, job.id, result);
		return;
	}
	web_api_reply_accepted(call, job.id, COMMISSIONING_URL);
}

void v1_get_onboarding_codes(struct web_api_call *call)
{
	struct matter_codes codes;
	struct web_json_writer *w = web_api_json(call);
	bool available;

	matter_service_get_codes(&codes);
	available = codes.reason == MATTER_CODES_AVAILABLE;

	web_json_object_begin(w);
	web_json_key(w, "available");
	web_json_bool(w, available);
	web_json_key(w, "reason");
	web_json_string_or_null(w, matter_codes_reason_str(codes.reason));
	web_json_key(w, "qr_payload");
	web_json_string_or_null(w, available ? codes.qr_payload : NULL);
	web_json_key(w, "manual_pairing_code");
	web_json_string_or_null(w, available ? codes.manual_pairing_code : NULL);
	web_json_key(w, "setup_passcode");
	web_json_string_or_null(w, available ? codes.setup_passcode : NULL);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

void v1_list_fabrics(struct web_api_call *call)
{
	static struct matter_fabric fabrics[MATTER_SERVICE_MAX_FABRICS];
	size_t count = matter_service_get_fabrics(fabrics, ARRAY_SIZE(fabrics));
	struct web_json_writer *w = web_api_json(call);
	char hex[17];

	web_json_object_begin(w);
	web_json_key(w, "items");
	web_json_array_begin(w);
	for (size_t i = 0; i < count; i++) {
		const struct matter_fabric *f = &fabrics[i];

		web_json_object_begin(w);
		web_json_key(w, "id");
		web_json_string(w, f->id);
		web_json_key(w, "fabric_index");
		web_json_int(w, f->fabric_index);
		web_json_key(w, "fabric_id");
		snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)f->fabric_id);
		web_json_string(w, hex);
		web_json_key(w, "node_id");
		snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)f->node_id);
		web_json_string(w, hex);
		web_json_key(w, "vendor_id");
		web_json_int(w, f->vendor_id);
		web_json_key(w, "label");
		web_json_string(w, f->label);
		web_json_object_end(w);
	}
	web_json_array_end(w);
	web_json_key(w, "count");
	web_json_int(w, (int64_t)count);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}
