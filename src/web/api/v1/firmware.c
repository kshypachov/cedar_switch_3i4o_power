/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1: the staged firmware images, the coprocessor's install and job
 * cancellation - createUpload, getUpload, writeUploadChunk, verifyUpload,
 * deleteUpload, startCoprocessorUpdate, cancelJob.
 *
 * Contract: "Upload и ESP32 update", "Обновление STM32" and "Задачи" in
 * api-contract.md, the Upload, FirmwareImage and JobAccepted schemas in
 * openapi.json. The decisions are firmware-store's (modules/firmware-store/README.md)
 * and system-image-store's (modules/system-image-store/README.md); this file maps
 * them onto HTTP. What the binding itself decides:
 *
 * - **Two stores, one resource.** An upload's `target` picks the store:
 *   esp32c6 (the default) is a file in firmware-store, stm32u585 goes straight
 *   into MCUboot's slot 2 through system-image-store. Afterwards the id says
 *   which store holds it (system-image-store's start "sysimg_"), so get, chunk,
 *   verify and delete need no target. The two stores keep the same call shapes
 *   and errno meanings; the answers differ only where the contract does.
 * - **One upload across both targets**: creating one while the other store
 *   holds an upload that has not failed is 409 busy. For stm32u585 the order is
 *   busy, then invalid_state (the running firmware is unconfirmed or a swap is
 *   pending: slot 2 holds what a revert needs), then 413 ("Решения этапа").
 * - **Each store must have been opened by the board** (fw_store_init() then
 *   web_api_v1_set_firmware(); sys_img_init() and system_updater_init() then
 *   web_api_v1_set_system()). Until then its operations answer 503
 *   service_not_ready rather than touching a store that is not there.
 * - **createUpload replays by key**, the way stageNetworkConfig does: it creates
 *   no job, so the binding keeps the scoped key and the upload it created (8
 *   entries, 15 minutes). A retry with the same key and body gets that upload
 *   again as it is now - or 404 if it was deleted meanwhile - never `busy` and
 *   never a second upload; the same key with another body is
 *   `idempotency_conflict`. cancelJob, which creates no job either, replays the
 *   same way.
 * - **Chunks and checks replay through job-manager**: the key is looked up
 *   before the chunk is staged, so a retry is answered with the original job and
 *   its bytes are never staged twice.
 * - **One worker**: every store call that does file or flash I/O (commit,
 *   verify, delete) runs on the v1 job worker (auth.c), which is the single
 *   thread both stores require. Each operation has one work item; the stores'
 *   pending-chunk and verifying flags keep a second one from being accepted
 *   while the first has not run.
 * - **A check that finds the image wanting fails its job** with the image's
 *   code, and the upload is `failed` with code and message, as the mock does.
 *   A cancelled check leaves the upload `receiving`.
 * - **cancelJob** cancels what job-manager allows: a check, an install before
 *   its point of no return (the coprocessor's `begin`, the STM32's
 *   `requesting`). Everything else - a chunk, a delete, a terminal job - is
 *   409 invalid_state; a network apply is cancelled through its transaction.
 * - **startCoprocessorUpdate** refuses in the contract's order ("Решения P6"):
 *   `ota` 503, not over Ethernet 409, unknown upload 404 (an STM32 upload 422
 *   unsupported_target), an install running 409 busy, upload not `ready` 409
 *   invalid_state, no acknowledge_recovery 422, UART with the USB bridge 409
 *   busy, UART unavailable 503. A replay by key is answered first, with the
 *   install's job. The install runs on the same v1 worker as firmware-store's
 *   file I/O, because the updater reads the staged image through the store. The
 *   C6's own state never refuses an install. startSystemUpdate is system_update.c.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <firmware_store/firmware_store.h>
#if defined(CONFIG_COPROCESSOR_UPDATER)
#include <coprocessor_updater/coprocessor_updater.h>
#endif
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
#include <system_image_store/system_image_store.h>
#endif
#if defined(CONFIG_SYSTEM_UPDATER)
#include <system_updater/system_updater.h>
#endif

#include "v1_internal.h"

LOG_MODULE_REGISTER(web_api_v1_firmware, LOG_LEVEL_INF);

#define UPLOADS_URL WEB_API_BASE_PATH "/firmware/uploads"

#if defined(CONFIG_SYSTEM_IMAGE_STORE)
BUILD_ASSERT(SYS_IMG_ID_LEN <= FW_STORE_ID_LEN, "the id buffers below are firmware-store's");
#endif

static const struct web_api_v1_firmware *fw_hooks;

static void forget_replays_and_jobs(void);

void web_api_v1_set_firmware(const struct web_api_v1_firmware *firmware)
{
	/* What the binding remembers - uploads to replay, which job worked on which
	 * upload - belongs to the store that was open; a store opened again starts
	 * with none of it. */
	forget_replays_and_jobs();
	fw_hooks = firmware;
}

static int64_t now_ms(void)
{
	return (fw_hooks != NULL && fw_hooks->now_ms != NULL) ? fw_hooks->now_ms() : k_uptime_get();
}

static bool store_ready(struct web_api_call *call)
{
	if (fw_hooks == NULL) {
		web_api_reject(call, API_ERR_SERVICE_NOT_READY, "The firmware store is not ready");
		return false;
	}
	return true;
}

/* An id of system-image-store, whatever state that store is in. */
static bool is_system_id(const char *id)
{
	return id != NULL &&
	       strncmp(id, V1_SYSTEM_UPLOAD_PREFIX, strlen(V1_SYSTEM_UPLOAD_PREFIX)) == 0;
}

#if defined(CONFIG_SYSTEM_IMAGE_STORE)
static int64_t sys_now_ms(void)
{
	const struct web_api_v1_system *sys = v1_system();

	return (sys != NULL && sys->now_ms != NULL) ? sys->now_ms() : k_uptime_get();
}
#endif

static bool system_store_ready(struct web_api_call *call)
{
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	if (v1_system() != NULL) {
		return true;
	}
	web_api_reject(call, API_ERR_SERVICE_NOT_READY, "The STM32 image store is not ready");
#else
	web_api_reject(call, API_ERR_CAPABILITY_UNAVAILABLE, "This build has no STM32 updater");
#endif
	return false;
}

/* -- request schema ---------------------------------------------------------- */

static bool sha256_hex(const char *value)
{
	size_t n = 0;

	for (; value[n] != '\0'; n++) {
		const char c = value[n];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return n == 64U;
}

static const char *const upload_targets[] = {"esp32c6", "stm32u585", NULL};

static const struct web_json_field upload_fields[] = {
	{
		.name = "filename",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_upload_body, filename),
		.size = sizeof(((struct v1_upload_body *)0)->filename),
		.min_len = 1,
		.max_len = 128,
	},
	{
		.name = "size_bytes",
		.type = WEB_JSON_INT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_upload_body, size_bytes),
		.min = 1,
		.max = INT64_MAX,
	},
	{
		.name = "sha256",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_upload_body, sha256),
		.size = sizeof(((struct v1_upload_body *)0)->sha256),
		.max_len = sizeof(((struct v1_upload_body *)0)->sha256) - 1U,
		.format = sha256_hex,
	},
	{
		/* Optional: absent stays "", which is esp32c6. */
		.name = "target",
		.type = WEB_JSON_STRING,
		.offset = offsetof(struct v1_upload_body, target),
		.size = sizeof(((struct v1_upload_body *)0)->target),
		.enum_values = upload_targets,
	},
};

const struct web_json_object v1_upload_schema = {
	.fields = upload_fields,
	.field_count = ARRAY_SIZE(upload_fields),
};

const char *const v1_upload_chunk_query[] = {"offset", NULL};

/* -- replays of requests that create no job ------------------------------------ */

#define REPLAY_SLOTS  8
#define REPLAY_TTL_MS (15 * 60 * 1000)

struct replay {
	bool used;
	char key[WEB_API_SCOPED_KEY_MAX_LEN + 1];
	uint32_t hash;
	/* The upload created, or the job cancelled. */
	char id[MAX(FW_STORE_ID_LEN, JOB_ID_MAX_LEN) + 1];
	int64_t at_ms;
};

/* Only the HTTP server's thread reaches this table. */
static struct replay replays[REPLAY_SLOTS];

/*
 * The request's earlier answer: false with @p id empty when there is none,
 * false with @p id set when there is one to repeat, true when a conflict has
 * been answered.
 */
static bool replay_find(struct web_api_call *call, char *id, size_t cap)
{
	const int64_t now = k_uptime_get();

	id[0] = '\0';
	for (size_t i = 0; i < ARRAY_SIZE(replays); i++) {
		struct replay *r = &replays[i];

		if (!r->used) {
			continue;
		}
		if (now - r->at_ms > REPLAY_TTL_MS) {
			r->used = false;
			continue;
		}
		if (strcmp(r->key, call->scoped_key) != 0) {
			continue;
		}
		if (r->hash != call->request_hash) {
			web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
				       "This Idempotency-Key was used with a different request");
			return true;
		}
		(void)snprintf(id, cap, "%s", r->id);
		return false;
	}
	return false;
}

static void replay_store(const struct web_api_call *call, const char *id)
{
	struct replay *slot = &replays[0];

	for (size_t i = 0; i < ARRAY_SIZE(replays); i++) {
		if (!replays[i].used) {
			slot = &replays[i];
			break;
		}
		/* Full: the oldest record gives way. */
		if (replays[i].at_ms < slot->at_ms) {
			slot = &replays[i];
		}
	}
	slot->used = true;
	(void)snprintf(slot->key, sizeof(slot->key), "%s", call->scoped_key);
	slot->hash = call->request_hash;
	(void)snprintf(slot->id, sizeof(slot->id), "%s", id);
	slot->at_ms = k_uptime_get();
}

/* -- which upload a job worked on ---------------------------------------------- */

#define JOB_UPLOADS 8

static struct {
	char job[JOB_ID_MAX_LEN + 1];
	char upload[FW_STORE_ID_LEN + 1];
} job_uploads[JOB_UPLOADS];
static size_t job_uploads_next;
static K_MUTEX_DEFINE(job_uploads_lock);

static void remember_job(const char *job_id, const char *upload_id)
{
	k_mutex_lock(&job_uploads_lock, K_FOREVER);
	(void)snprintf(job_uploads[job_uploads_next].job, sizeof(job_uploads[0].job), "%s",
		       job_id);
	(void)snprintf(job_uploads[job_uploads_next].upload, sizeof(job_uploads[0].upload), "%s",
		       upload_id);
	job_uploads_next = (job_uploads_next + 1U) % JOB_UPLOADS;
	k_mutex_unlock(&job_uploads_lock);
}

static void forget_replays_and_jobs(void)
{
	memset(replays, 0, sizeof(replays));
	k_mutex_lock(&job_uploads_lock, K_FOREVER);
	memset(job_uploads, 0, sizeof(job_uploads));
	job_uploads_next = 0;
	k_mutex_unlock(&job_uploads_lock);
}

const char *v1_upload_job_resource_url(const struct job_snapshot *job, char *buf, size_t cap)
{
	const char *url = NULL;

	k_mutex_lock(&job_uploads_lock, K_FOREVER);
	for (size_t i = 0; i < JOB_UPLOADS; i++) {
		if (job_uploads[i].job[0] != '\0' && strcmp(job_uploads[i].job, job->id) == 0) {
			(void)snprintf(buf, cap, UPLOADS_URL "/%s", job_uploads[i].upload);
			url = buf;
			break;
		}
	}
	k_mutex_unlock(&job_uploads_lock);

	return url;
}

/* -- responses ------------------------------------------------------------------ */

/* An Upload as either store describes it: what the reply needs, borrowed. */
struct upload_view {
	const char *id;
	const char *filename;
	uint32_t size_bytes;
	uint32_t received_bytes;
	const uint8_t *sha256;
	const char *state;
	const char *active_job_id;
	bool stm32;
	/** Ready with a checked image. */
	bool has_image;
	/** Failed with a code. */
	bool has_error;
	const char *version;
	const char *layout_id;
	const char *host_protocol;
	const char *error_code;
	const char *error_message;
};

static void view_of_fw(const struct fw_upload *up, struct upload_view *v)
{
	*v = (struct upload_view){
		.id = up->id,
		.filename = up->filename,
		.size_bytes = up->size_bytes,
		.received_bytes = up->received_bytes,
		.sha256 = up->sha256,
		.state = fw_upload_state_str(up->state),
		.active_job_id = up->active_job_id,
		.stm32 = false,
		.has_image = up->state == FW_UPLOAD_READY && up->has_image,
		.has_error = up->state == FW_UPLOAD_FAILED && up->error_code[0] != '\0',
		.version = up->version,
		.layout_id = up->layout_id,
		.host_protocol = up->host_protocol,
		.error_code = up->error_code,
		.error_message = up->error_message,
	};
}

#if defined(CONFIG_SYSTEM_IMAGE_STORE)
static void view_of_sys(const struct sys_img_upload *up, struct upload_view *v)
{
	*v = (struct upload_view){
		.id = up->id,
		.filename = up->filename,
		.size_bytes = up->size_bytes,
		.received_bytes = up->received_bytes,
		.sha256 = up->sha256,
		.state = sys_img_state_str(up->state),
		.active_job_id = up->active_job_id,
		.stm32 = true,
		.has_image = up->state == SYS_IMG_READY && up->has_image,
		.has_error = up->state == SYS_IMG_FAILED && up->error_code[0] != '\0',
		.version = up->version,
		.layout_id = NULL,
		.host_protocol = NULL,
		.error_code = up->error_code,
		.error_message = up->error_message,
	};
}
#endif

static const char *hex(const uint8_t *bytes, size_t n, char *out)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[bytes[i] >> 4];
		out[2 * i + 1] = digits[bytes[i] & 0x0f];
	}
	out[2 * n] = '\0';
	return out;
}

static void write_image(struct web_json_writer *w, const struct upload_view *up)
{
	web_json_object_begin(w);
	web_json_key(w, "format");
	web_json_string(w, up->stm32 ? "mcuboot_image" : "raw_full_flash");
	web_json_key(w, "format_version");
	web_json_null(w);
	web_json_key(w, "target");
	web_json_string(w, up->stm32 ? "stm32u585" : "esp32c6");
	web_json_key(w, "version");
	web_json_string(w, up->version);
	/* The merged file replaces the chip's bootloader and table and erases its
	 * NVS; the STM32 image is an application MCUboot swaps in. */
	web_json_key(w, "kind");
	web_json_string(w, up->stm32 ? "app" : "recovery_bundle");
	web_json_key(w, "partition_layout_id");
	web_json_string_or_null(w, up->layout_id);
	web_json_key(w, "host_protocol");
	web_json_string_or_null(w, up->host_protocol);
	/* A raw file carries no signature: null, not a check that failed. */
	web_json_key(w, "signature_verified");
	web_json_null(w);
	web_json_key(w, "allowed_methods");
	web_json_array_begin(w);
	web_json_string(w, up->stm32 ? "ota" : "uart");
	web_json_array_end(w);
	web_json_object_end(w);
}

static void reply_upload(struct web_api_call *call, const struct upload_view *up, uint16_t status)
{
	struct web_json_writer *w = web_api_json(call);
	char digest[65];

	web_json_object_begin(w);
	web_json_key(w, "id");
	web_json_string(w, up->id);
	web_json_key(w, "target");
	web_json_string(w, up->stm32 ? "stm32u585" : "esp32c6");
	web_json_key(w, "filename");
	web_json_string(w, up->filename);
	web_json_key(w, "size_bytes");
	web_json_int(w, up->size_bytes);
	web_json_key(w, "received_bytes");
	web_json_int(w, up->received_bytes);
	web_json_key(w, "sha256");
	web_json_string(w, hex(up->sha256, 32, digest));
	web_json_key(w, "state");
	web_json_string(w, up->state);
	web_json_key(w, "active_job_id");
	web_json_string_or_null(w, up->active_job_id[0] != '\0' ? up->active_job_id : NULL);
	web_json_key(w, "image");
	if (up->has_image) {
		write_image(w, up);
	} else {
		web_json_null(w);
	}
	web_json_key(w, "error");
	if (up->has_error) {
		web_json_object_begin(w);
		web_json_key(w, "code");
		web_json_string(w, up->error_code);
		web_json_key(w, "message");
		web_json_string(w, up->error_message);
		web_json_key(w, "request_id");
		web_json_string(w, call->ctx->rsp.request_id);
		web_json_key(w, "retryable");
		web_json_bool(w, false);
		web_json_object_end(w);
	} else {
		web_json_null(w);
	}
	web_json_object_end(w);

	if (status == 201) {
		char location[sizeof(UPLOADS_URL "/") + FW_STORE_ID_LEN];

		(void)snprintf(location, sizeof(location), UPLOADS_URL "/%s", up->id);
		web_api_set_location(call, location);
	}
	web_api_reply_json(call, status);
}

/* The upload named @p id in whichever store holds it; false when neither does. */
static bool find_upload(const char *id, struct upload_view *view)
{
	static struct fw_upload fw;

	if (is_system_id(id)) {
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
		static struct sys_img_upload sys;

		if (v1_system() != NULL && sys_img_get(id, sys_now_ms(), &sys) == 0) {
			view_of_sys(&sys, view);
			return true;
		}
#endif
		return false;
	}
	if (fw_hooks != NULL && fw_store_get(id, now_ms(), &fw) == 0) {
		view_of_fw(&fw, view);
		return true;
	}
	return false;
}

static void reply_accepted_upload(struct web_api_call *call, const char *job_id,
				  const char *upload_id)
{
	char url[sizeof(UPLOADS_URL "/") + FW_STORE_ID_LEN];

	(void)snprintf(url, sizeof(url), UPLOADS_URL "/%s", upload_id);
	web_api_reply_accepted(call, job_id, url);
}

/* A job could not be created: 429 when job-manager is full, 500 otherwise. */
static void reject_job_create(struct web_api_call *call, enum job_create_result result)
{
	if (result == JOB_CREATE_EXHAUSTED) {
		(void)api_error_init(web_api_error(call), API_ERR_RATE_LIMITED,
				     "Too many operations are in flight; retry shortly", NULL);
		(void)api_error_set_retry_after(web_api_error(call), 1);
		web_api_reject_error(call);
	} else {
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The job could not be created");
	}
}

/* -- the worker's side ------------------------------------------------------------ */

/*
 * What a work item works on, set by the HTTP thread before it submits and
 * copied by the worker when it starts. One item per operation: the store
 * refuses a second chunk, check or delete of its upload until the first has run.
 * One item serves both stores: a second upload cannot exist while the first does.
 */
struct fw_work {
	struct k_work work;
	char job[JOB_ID_MAX_LEN + 1];
	char upload[FW_STORE_ID_LEN + 1];
	size_t bytes;
	/* A check whose job could not be created: undo the claim, nothing else. */
	bool abandon;
	/* The upload is system-image-store's. */
	bool stm32;
#if defined(CONFIG_SYSTEM_IMAGE_STORE_TIMING)
	/* When the chunk was queued, to log how long it waited for the worker. */
	uint32_t queued_cyc;
#endif
};

static K_MUTEX_DEFINE(work_lock);

static void take(struct fw_work *fw, struct fw_work *out)
{
	k_mutex_lock(&work_lock, K_FOREVER);
	memcpy(out->job, fw->job, sizeof(out->job));
	memcpy(out->upload, fw->upload, sizeof(out->upload));
	out->bytes = fw->bytes;
	out->abandon = fw->abandon;
	out->stm32 = fw->stm32;
#if defined(CONFIG_SYSTEM_IMAGE_STORE_TIMING)
	out->queued_cyc = fw->queued_cyc;
#endif
	k_mutex_unlock(&work_lock);
}

static int submit(struct fw_work *fw, const char *job_id, const char *upload_id, size_t bytes,
		  bool abandon)
{
	k_mutex_lock(&work_lock, K_FOREVER);
	(void)snprintf(fw->job, sizeof(fw->job), "%s", job_id);
	(void)snprintf(fw->upload, sizeof(fw->upload), "%s", upload_id);
	fw->bytes = bytes;
	fw->abandon = abandon;
	fw->stm32 = is_system_id(upload_id);
#if defined(CONFIG_SYSTEM_IMAGE_STORE_TIMING)
	fw->queued_cyc = k_cycle_get_32();
#endif
	k_mutex_unlock(&work_lock);

	return v1_worker_submit(&fw->work);
}

/* The store calls of the worker, by the upload's store. */
static int store_commit(const struct fw_work *op)
{
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	if (op->stm32) {
		return sys_img_chunk_commit(op->upload, sys_now_ms());
	}
#endif
	return fw_store_chunk_commit(op->upload, now_ms());
}

static void store_clear_job(const struct fw_work *op)
{
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	if (op->stm32) {
		(void)sys_img_set_active_job(op->upload, NULL);
		return;
	}
#endif
	(void)fw_store_set_active_job(op->upload, NULL);
}

static void run_chunk(struct k_work *work)
{
	struct fw_work op;
	int rc;

	take(CONTAINER_OF(work, struct fw_work, work), &op);
#if defined(CONFIG_SYSTEM_IMAGE_STORE_TIMING)
	const uint32_t started = k_cycle_get_32();
#endif
	(void)job_set_state(op.job, JOB_STATE_RUNNING);
	(void)job_set_phase(op.job, "writing");
	(void)job_set_progress(op.job, 0U, op.bytes, true, JOB_PROGRESS_UNIT_BYTES);

	rc = store_commit(&op);
	store_clear_job(&op);
#if defined(CONFIG_SYSTEM_IMAGE_STORE_TIMING)
	LOG_INF("chunk job %s: queued %u us, ran %u us", op.job,
		k_cyc_to_us_floor32(started - op.queued_cyc),
		k_cyc_to_us_floor32(k_cycle_get_32() - started));
#endif
	if (rc == 0) {
		(void)job_set_progress(op.job, op.bytes, op.bytes, true, JOB_PROGRESS_UNIT_BYTES);
		(void)job_set_state(op.job, JOB_STATE_SUCCEEDED);
	} else {
		/* The offset did not move: the client sends the same chunk again. */
		LOG_WRN("chunk of %s not written: %d", op.upload, rc);
		(void)job_fail(op.job, rc == -ENOSPC ? "storage_full" : "internal_error",
			       rc != -ENOSPC);
	}
}

static bool never(void *ctx)
{
	ARG_UNUSED(ctx);
	return true;
}

static bool verify_cancelled(void *ctx)
{
	struct job_snapshot job;

	return job_get((const char *)ctx, &job) != 0 || job.state == JOB_STATE_CANCELLED;
}

static void verify_progress(void *ctx, uint32_t done, uint32_t total)
{
	(void)job_set_progress((const char *)ctx, done, total, true, JOB_PROGRESS_UNIT_BYTES);
}

static void run_verify(struct k_work *work)
{
	struct fw_work op;
	int rc;

	take(CONTAINER_OF(work, struct fw_work, work), &op);
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	if (op.stm32) {
		if (op.abandon) {
			(void)sys_img_verify(op.upload, never, NULL, NULL, sys_now_ms());
			(void)sys_img_set_active_job(op.upload, NULL);
			return;
		}
		(void)job_set_state(op.job, JOB_STATE_RUNNING);
		(void)job_set_phase(op.job, "verifying");
		rc = sys_img_verify(op.upload, verify_cancelled, verify_progress, op.job,
				    sys_now_ms());
		(void)sys_img_set_active_job(op.upload, NULL);
	} else
#endif
	{
		if (op.abandon) {
			/* Stops at the first question and puts the upload back to receiving. */
			(void)fw_store_verify(op.upload, never, NULL, NULL, now_ms());
			(void)fw_store_set_active_job(op.upload, NULL);
			return;
		}
		/* Refused when the job was cancelled while queued; the check then stops
		 * at its first question. */
		(void)job_set_state(op.job, JOB_STATE_RUNNING);
		(void)job_set_phase(op.job, "verifying");
		rc = fw_store_verify(op.upload, verify_cancelled, verify_progress, op.job, now_ms());
		(void)fw_store_set_active_job(op.upload, NULL);
	}
	if (rc == -ECANCELED) {
		return;
	}
	/* Not find_upload(): its buffers are the HTTP thread's. The worker's own copy. */
	bool ready = false;
	char code[FW_STORE_ERROR_CODE_MAX + 1] = "";

	if (rc == 0) {
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
		if (op.stm32) {
			struct sys_img_upload sys;

			rc = sys_img_get(op.upload, sys_now_ms(), &sys);
			ready = rc == 0 && sys.state == SYS_IMG_READY;
			(void)snprintf(code, sizeof(code), "%s", rc == 0 ? sys.error_code : "");
		} else
#endif
		{
			struct fw_upload fw;

			rc = fw_store_get(op.upload, now_ms(), &fw);
			ready = rc == 0 && fw.state == FW_UPLOAD_READY;
			(void)snprintf(code, sizeof(code), "%s", rc == 0 ? fw.error_code : "");
		}
	}
	if (rc != 0) {
		LOG_ERR("check of %s: %d", op.upload, rc);
		(void)job_fail(op.job, "internal_error", true);
		return;
	}
	if (ready) {
		(void)job_set_state(op.job, JOB_STATE_SUCCEEDED);
	} else {
		(void)job_fail(op.job, code[0] != '\0' ? code : "invalid_image", false);
	}
}

static void run_delete(struct k_work *work)
{
	struct fw_work op;
	int rc;

	take(CONTAINER_OF(work, struct fw_work, work), &op);
	(void)job_set_state(op.job, JOB_STATE_RUNNING);
	(void)job_set_phase(op.job, "deleting");

#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	rc = op.stm32 ? sys_img_delete(op.upload) : fw_store_delete(op.upload);
#else
	rc = fw_store_delete(op.upload);
#endif
	if (rc == 0 || rc == -ENOENT) {
		(void)job_set_state(op.job, JOB_STATE_SUCCEEDED);
	} else {
		store_clear_job(&op);
		(void)job_fail(op.job, rc == -EBUSY ? "busy" : "internal_error", rc == -EBUSY);
	}
}

static struct fw_work chunk_work = {.work = Z_WORK_INITIALIZER(run_chunk)};
static struct fw_work verify_work = {.work = Z_WORK_INITIALIZER(run_verify)};
static struct fw_work delete_work = {.work = Z_WORK_INITIALIZER(run_delete)};

/* -- handlers ----------------------------------------------------------------------- */

static void parse_digest(const char *text, uint8_t digest[32])
{
	for (size_t i = 0; i < 32U; i++) {
		const char h = text[2 * i];
		const char l = text[2 * i + 1];

		digest[i] = (uint8_t)(((h <= '9' ? h - '0' : h - 'a' + 10) << 4) |
				      (l <= '9' ? l - '0' : l - 'a' + 10));
	}
}

/*
 * The other target's store holds an upload that has not failed; its id into @p id.
 * The id goes into the refusal: the page of one processor does not know the other
 * page's upload (it may come from another browser), and without the id nobody can
 * delete it short of its 24-hour expiry.
 */
static bool other_target_busy(bool stm32, char *id, size_t cap)
{
	if (stm32) {
		static struct fw_upload fw;

		if (fw_hooks != NULL && fw_store_current(now_ms(), &fw) == 0 &&
		    fw.state != FW_UPLOAD_FAILED) {
			(void)snprintf(id, cap, "%s", fw.id);
			return true;
		}
		return false;
	}
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	static struct sys_img_upload sys;

	if (v1_system() != NULL && sys_img_current(sys_now_ms(), &sys) == 0 &&
	    sys.state != SYS_IMG_FAILED) {
		(void)snprintf(id, cap, "%s", sys.id);
		return true;
	}
#endif
	return false;
}

static void reply_replayed_upload(struct web_api_call *call, const char *id)
{
	struct upload_view view;

	if (find_upload(id, &view)) {
		reply_upload(call, &view, 201);
	} else {
		web_api_reject(call, API_ERR_NOT_FOUND,
			       "The upload this request created no longer exists");
	}
}

static const char MSG_BUSY_UPLOAD[] = "An upload is already staged or installing; delete it first";

#if defined(CONFIG_SYSTEM_IMAGE_STORE)
static void create_system_upload(struct web_api_call *call, const struct v1_upload_body *body)
{
	static struct sys_img_upload up;
	struct upload_view view;
	uint8_t digest[32];
	int rc;

#if defined(CONFIG_SYSTEM_UPDATER)
	/* busy (the other store, below), then invalid_state, then 413. */
	rc = system_updater_check();
#else
	rc = 0;
#endif
	if (rc == -EACCES) {
		web_api_reject(call, API_ERR_INVALID_STATE,
			       "The running firmware is not confirmed yet, or a swap is pending: "
			       "slot 2 holds the firmware a revert would restore");
		return;
	}
	if (body->size_bytes > (int64_t)v1_system_upload_max_bytes()) {
		web_api_reject(call, API_ERR_PAYLOAD_TOO_LARGE,
			       "The image is larger than MCUboot's slot allows");
		return;
	}
	parse_digest(body->sha256, digest);

	rc = sys_img_create(body->filename, (uint32_t)body->size_bytes, digest, sys_now_ms(), &up);
	switch (rc) {
	case 0:
		break;
	case -EBUSY:
		web_api_reject(call, API_ERR_BUSY, MSG_BUSY_UPLOAD);
		return;
	case -EACCES:
		web_api_reject(call, API_ERR_INVALID_STATE,
			       "The running firmware is not confirmed yet, or a swap is pending: "
			       "slot 2 holds the firmware a revert would restore");
		return;
	case -EFBIG:
		web_api_reject(call, API_ERR_PAYLOAD_TOO_LARGE,
			       "The image is larger than MCUboot's slot allows");
		return;
	case -EINVAL:
		web_api_reject(call, API_ERR_VALIDATION_FAILED, "The upload request is not acceptable");
		return;
	case -EAGAIN:
		web_api_reject(call, API_ERR_SERVICE_NOT_READY, "The STM32 image store is not ready");
		return;
	default:
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The upload could not be recorded");
		return;
	}

	replay_store(call, up.id);
	view_of_sys(&up, &view);
	reply_upload(call, &view, 201);
}
#endif

void v1_create_upload(struct web_api_call *call)
{
	const struct v1_upload_body *body = call->body;
	const bool stm32 = strcmp(body->target, "stm32u585") == 0;
	static struct fw_upload up;
	struct upload_view view;
	uint8_t digest[32];
	char id[FW_STORE_ID_LEN + 1];
	int rc;

	if (stm32 ? !system_store_ready(call) : !store_ready(call)) {
		return;
	}
	if (replay_find(call, id, sizeof(id))) {
		return;
	}
	if (id[0] != '\0') {
		reply_replayed_upload(call, id);
		return;
	}
	if (other_target_busy(stm32, id, sizeof(id))) {
		char message[128];

		(void)snprintf(message, sizeof(message),
			       "Upload %s of the %s is staged; delete it first", id,
			       stm32 ? "ESP32" : "STM32");
		web_api_reject(call, API_ERR_BUSY, message);
		return;
	}
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	if (stm32) {
		create_system_upload(call, body);
		return;
	}
#endif
	if (body->size_bytes > (int64_t)CONFIG_FIRMWARE_STORE_MAX_BYTES) {
		web_api_reject(call, API_ERR_PAYLOAD_TOO_LARGE,
			       "The image is larger than the coprocessor's application slot allows");
		return;
	}
	parse_digest(body->sha256, digest);

	rc = fw_store_create(body->filename, (uint32_t)body->size_bytes, digest, now_ms(), &up);
	switch (rc) {
	case 0:
		break;
	case -EBUSY:
		web_api_reject(call, API_ERR_BUSY, MSG_BUSY_UPLOAD);
		return;
	case -EFBIG:
		web_api_reject(call, API_ERR_PAYLOAD_TOO_LARGE,
			       "The image is larger than the coprocessor's application slot allows");
		return;
	case -ENOSPC:
		web_api_reject(call, API_ERR_STORAGE_FULL,
			       "Not enough free space on the device for this image");
		return;
	case -EINVAL:
		web_api_reject(call, API_ERR_VALIDATION_FAILED, "The upload request is not acceptable");
		return;
	case -EAGAIN:
		web_api_reject(call, API_ERR_SERVICE_NOT_READY, "The firmware store is not ready");
		return;
	default:
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The upload could not be recorded");
		return;
	}

	replay_store(call, up.id);
	view_of_fw(&up, &view);
	reply_upload(call, &view, 201);
}

/* The upload's store must be open: 503 otherwise, as before an upload exists. */
static bool upload_store_ready(struct web_api_call *call, const char *upload_id)
{
	return is_system_id(upload_id) ? system_store_ready(call) : store_ready(call);
}

void v1_get_upload(struct web_api_call *call)
{
	struct upload_view view;

	if (!upload_store_ready(call, call->params[0])) {
		return;
	}
	if (!find_upload(call->params[0], &view)) {
		web_api_reject(call, API_ERR_NOT_FOUND, "No such upload");
		return;
	}
	reply_upload(call, &view, 200);
}

/* A decimal integer that fits 32 bits: no sign, no spaces, at least one digit. */
static bool parse_offset(const struct web_api_request *req, uint32_t *out)
{
	char text[16];
	uint64_t v = 0;
	int n = web_api_query_get(req, "offset", text, sizeof(text));

	if (n <= 0 || n > 10) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		if (text[i] < '0' || text[i] > '9') {
			return false;
		}
		v = v * 10U + (uint64_t)(text[i] - '0');
	}
	if (v > UINT32_MAX) {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

static int chunk_accept(const char *upload_id, uint32_t offset, const uint8_t *data, size_t len)
{
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	if (is_system_id(upload_id)) {
		return sys_img_chunk_accept(upload_id, offset, data, len, sys_now_ms());
	}
#endif
	return fw_store_chunk_accept(upload_id, offset, data, len, now_ms());
}

static void chunk_discard(const char *upload_id)
{
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	if (is_system_id(upload_id)) {
		sys_img_chunk_discard(upload_id);
		return;
	}
#endif
	fw_store_chunk_discard(upload_id);
}

static void set_active_job(const char *upload_id, const char *job_id)
{
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	if (is_system_id(upload_id)) {
		(void)sys_img_set_active_job(upload_id, job_id);
		return;
	}
#endif
	(void)fw_store_set_active_job(upload_id, job_id);
}

void v1_write_upload_chunk(struct web_api_call *call)
{
	const char *upload_id = call->params[0];
	const struct job_create_params params = {
		.kind = JOB_KIND_UPLOAD_CHUNK,
		.cancellable = false,
		.idempotency_key = call->scoped_key,
		.request_hash = call->request_hash,
	};
	struct job_snapshot job;
	enum job_create_result created;
	uint32_t offset;
	int rc;

	if (!upload_store_ready(call, upload_id)) {
		return;
	}
	if (!parse_offset(call->req, &offset)) {
		web_api_reject(call, API_ERR_INVALID_QUERY,
			       "offset must be a non-negative decimal integer");
		return;
	}
	/* A retry of an accepted chunk is its job, and its bytes are not staged again. */
	switch (job_find_by_key(call->scoped_key, call->request_hash, &job)) {
	case JOB_LOOKUP_EXISTING:
		reply_accepted_upload(call, job.id, upload_id);
		return;
	case JOB_LOOKUP_CONFLICT:
		web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
			       "This Idempotency-Key was used with a different request");
		return;
	default:
		break;
	}

	rc = chunk_accept(upload_id, offset, call->octets, call->octets_len);
	switch (rc) {
	case 0:
		break;
	case -ENOENT:
		web_api_reject(call, API_ERR_NOT_FOUND, "No such upload");
		return;
	case -EACCES:
		web_api_reject(call, API_ERR_INVALID_STATE,
			       "The running firmware is not confirmed yet, or a swap is pending: "
			       "slot 2 is not free to overwrite");
		return;
	case -EINVAL:
		web_api_reject(call, API_ERR_INVALID_STATE, "This upload takes no more data");
		return;
	case -EBUSY:
		web_api_reject(call, API_ERR_BUSY, "The previous chunk is still being written");
		return;
	case -ERANGE: {
		struct upload_view view;
		char message[64];

		(void)snprintf(message, sizeof(message), "The next acceptable offset is %u",
			       find_upload(upload_id, &view) ? (unsigned int)view.received_bytes : 0U);
		web_api_reject(call, API_ERR_OFFSET_MISMATCH, message);
		return;
	}
	case -E2BIG:
		web_api_reject(call, API_ERR_PAYLOAD_TOO_LARGE, "A chunk may not exceed the chunk limit");
		return;
	case -ENODATA:
		web_api_reject(call, API_ERR_VALIDATION_FAILED, "An empty chunk writes nothing");
		return;
	case -EOVERFLOW:
		web_api_reject(call, API_ERR_VALIDATION_FAILED, "The chunk runs past the declared size");
		return;
	default:
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The chunk could not be taken");
		return;
	}

	created = job_create(&params, &job);
	if (created != JOB_CREATE_NEW) {
		chunk_discard(upload_id);
		reject_job_create(call, created);
		return;
	}
	set_active_job(upload_id, job.id);
	remember_job(job.id, upload_id);
	if (submit(&chunk_work, job.id, upload_id, call->octets_len, false) < 0) {
		chunk_discard(upload_id);
		set_active_job(upload_id, NULL);
		(void)job_fail(job.id, "internal_error", true);
	}
	reply_accepted_upload(call, job.id, upload_id);
}

void v1_verify_upload(struct web_api_call *call)
{
	const char *upload_id = call->params[0];
	const struct job_create_params params = {
		.kind = JOB_KIND_FIRMWARE_VERIFY,
		.cancellable = true,
		.idempotency_key = call->scoped_key,
		.request_hash = call->request_hash,
	};
	struct job_snapshot job;
	enum job_create_result created;
	int rc;

	if (!upload_store_ready(call, upload_id)) {
		return;
	}
	switch (job_find_by_key(call->scoped_key, call->request_hash, &job)) {
	case JOB_LOOKUP_EXISTING:
		reply_accepted_upload(call, job.id, upload_id);
		return;
	case JOB_LOOKUP_CONFLICT:
		web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
			       "This Idempotency-Key was used with a different request");
		return;
	default:
		break;
	}

#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	rc = is_system_id(upload_id) ? sys_img_verify_begin(upload_id, sys_now_ms())
				     : fw_store_verify_begin(upload_id, now_ms());
#else
	rc = fw_store_verify_begin(upload_id, now_ms());
#endif
	switch (rc) {
	case 0:
		break;
	case -ENOENT:
		web_api_reject(call, API_ERR_NOT_FOUND, "No such upload");
		return;
	case -EBUSY:
		web_api_reject(call, API_ERR_BUSY, "A chunk is still being written or an install uses the file");
		return;
	default:
		web_api_reject(call, API_ERR_INVALID_STATE,
			       "The upload is incomplete, being checked, or already checked");
		return;
	}

	created = job_create(&params, &job);
	if (created != JOB_CREATE_NEW) {
		/* The claim is the worker's to undo: it is the one thread that may
		 * run the check, and a check told to stop leaves `receiving`. */
		(void)submit(&verify_work, "", upload_id, 0U, true);
		reject_job_create(call, created);
		return;
	}
	set_active_job(upload_id, job.id);
	remember_job(job.id, upload_id);
	if (submit(&verify_work, job.id, upload_id, 0U, false) < 0) {
		(void)job_fail(job.id, "internal_error", true);
	}
	reply_accepted_upload(call, job.id, upload_id);
}

/* In use: an install holds it, a chunk is pending, or a check runs. */
static bool upload_in_use(const char *upload_id)
{
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	if (is_system_id(upload_id)) {
		static struct sys_img_upload sys;

		return sys_img_get(upload_id, sys_now_ms(), &sys) == 0 &&
		       (sys.in_use || sys.chunk_pending || sys.state == SYS_IMG_VERIFYING);
	}
#endif
	static struct fw_upload fw;

	return fw_store_get(upload_id, now_ms(), &fw) == 0 &&
	       (fw.in_use || fw.chunk_pending || fw.state == FW_UPLOAD_VERIFYING);
}

void v1_delete_upload(struct web_api_call *call)
{
	const char *upload_id = call->params[0];
	const struct job_create_params params = {
		.kind = JOB_KIND_FIRMWARE_DELETE,
		.cancellable = false,
		.idempotency_key = call->scoped_key,
		.request_hash = call->request_hash,
	};
	struct job_snapshot job;
	struct upload_view view;
	enum job_create_result created;

	if (!upload_store_ready(call, upload_id)) {
		return;
	}
	switch (job_find_by_key(call->scoped_key, call->request_hash, &job)) {
	case JOB_LOOKUP_EXISTING:
		web_api_reply_accepted(call, job.id, NULL);
		return;
	case JOB_LOOKUP_CONFLICT:
		web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
			       "This Idempotency-Key was used with a different request");
		return;
	default:
		break;
	}
	if (!find_upload(upload_id, &view)) {
		web_api_reject(call, API_ERR_NOT_FOUND, "No such upload");
		return;
	}
	/* Refused before a job exists, so a refusal leaves nothing under the key. */
	if (upload_in_use(upload_id) || k_work_is_pending(&delete_work.work)) {
		web_api_reject(call, API_ERR_BUSY, "The staged image is in use");
		return;
	}

	created = job_create(&params, &job);
	if (created != JOB_CREATE_NEW) {
		reject_job_create(call, created);
		return;
	}
	set_active_job(upload_id, job.id);
	if (submit(&delete_work, job.id, upload_id, 0U, false) < 0) {
		set_active_job(upload_id, NULL);
		(void)job_fail(job.id, "internal_error", true);
	}
	web_api_reply_accepted(call, job.id, NULL);
}

void v1_cancel_job(struct web_api_call *call)
{
	struct job_snapshot job;
	char id[JOB_ID_MAX_LEN + 1];
	char url[96];

	if (replay_find(call, id, sizeof(id))) {
		return;
	}
	if (id[0] != '\0') {
		if (job_get(id, &job) == 0) {
			web_api_reply_accepted(call, job.id, v1_job_resource_url(&job, url, sizeof(url)));
		} else {
			web_api_reject(call, API_ERR_NOT_FOUND, "No such job");
		}
		return;
	}
	if (job_get(call->params[0], &job) != 0) {
		web_api_reject(call, API_ERR_NOT_FOUND, "No such job");
		return;
	}
	if (job.kind == JOB_KIND_NETWORK_APPLY) {
		web_api_reject(call, API_ERR_INVALID_STATE,
			       "Cancel a network apply through DELETE on its transaction");
		return;
	}

	switch (job_cancel(job.id)) {
	case 0:
		break;
	case -ENOENT:
		web_api_reject(call, API_ERR_NOT_FOUND, "No such job");
		return;
	default:
		/* Not cancellable (a destructive phase), or already over. */
		web_api_reject(call, API_ERR_INVALID_STATE, "This job cannot be cancelled now");
		return;
	}

	replay_store(call, job.id);
	web_api_reply_accepted(call, job.id, v1_job_resource_url(&job, url, sizeof(url)));
}

/* -- startCoprocessorUpdate ---------------------------------------------------------- */

#define STATUS_URL WEB_API_BASE_PATH "/coprocessor/status"

static const char *const update_methods[] = {"ota", "uart", NULL};

static const struct web_json_field update_fields[] = {
	{
		.name = "upload_id",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_update_body, upload_id),
		.size = sizeof(((struct v1_update_body *)0)->upload_id),
		.min_len = 1,
		.max_len = 64,
		.format = api_validate_opaque_id,
	},
	{
		.name = "method",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_update_body, method),
		.size = sizeof(((struct v1_update_body *)0)->method),
		.enum_values = update_methods,
	},
	{
		.name = "acknowledge_recovery",
		.type = WEB_JSON_BOOL,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_update_body, acknowledge_recovery),
	},
};

const struct web_json_object v1_update_schema = {
	.fields = update_fields,
	.field_count = ARRAY_SIZE(update_fields),
};

#if defined(CONFIG_COPROCESSOR_UPDATER)
static void run_update(struct k_work *work)
{
	ARG_UNUSED(work);
	coprocessor_updater_run();
}

static K_WORK_DEFINE(update_work, run_update);
#endif

void v1_start_coprocessor_update(struct web_api_call *call)
{
	const struct v1_update_body *body = call->body;
	struct job_snapshot job;

	/* A retry of an accepted install is its job, whatever changed since. */
	switch (job_find_by_key(call->scoped_key, call->request_hash, &job)) {
	case JOB_LOOKUP_EXISTING:
		web_api_reply_accepted(call, job.id, STATUS_URL);
		return;
	case JOB_LOOKUP_CONFLICT:
		web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
			       "This Idempotency-Key was used with a different request");
		return;
	default:
		break;
	}

	if (strcmp(body->method, "ota") == 0) {
		web_api_reject(call, API_ERR_CAPABILITY_UNAVAILABLE,
			       "ESP32 OTA is not implemented in this version; use method \"uart\"");
		return;
	}
	/* Before any reset or erase: the UART write takes Wi-Fi down with the C6. */
	if (!v1_request_over_ethernet(call->req)) {
		web_api_reject(call, API_ERR_ETHERNET_REQUIRED,
			       "A coprocessor update must be requested over Ethernet");
		return;
	}
	if (!store_ready(call)) {
		return;
	}

#if !defined(CONFIG_COPROCESSOR_UPDATER)
	web_api_reject(call, API_ERR_CAPABILITY_UNAVAILABLE, "This build has no coprocessor updater");
#else
	const struct job_create_params params = {
		.kind = JOB_KIND_COPROCESSOR_UPDATE,
		/* The updater turns this off at `begin`. */
		.cancellable = true,
		.idempotency_key = call->scoped_key,
		.request_hash = call->request_hash,
	};
	struct coprocessor_updater_state st;
	struct coprocessor_status cp;
	static struct fw_upload up;
	enum job_create_result created;
	int rc;

	if (fw_store_get(body->upload_id, now_ms(), &up) != 0) {
		struct upload_view view;

		if (is_system_id(body->upload_id) && find_upload(body->upload_id, &view)) {
			web_api_reject(call, API_ERR_UNSUPPORTED_TARGET,
				       "This upload is an STM32 image; install it through "
				       "/system/updates");
		} else {
			web_api_reject(call, API_ERR_NOT_FOUND, "No such upload");
		}
		return;
	}
	coprocessor_updater_get_state(&st);
	if (st.active) {
		web_api_reject(call, API_ERR_BUSY, "A coprocessor update is already running");
		return;
	}
	if (up.state != FW_UPLOAD_READY) {
		web_api_reject(call, API_ERR_INVALID_STATE, "The upload is not verified and ready");
		return;
	}
	/* Every raw_full_flash file is a recovery bundle. */
	if (!body->acknowledge_recovery) {
		web_api_reject(call, API_ERR_VALIDATION_FAILED,
			       "This image replaces the coprocessor's bootloader and partition "
			       "table and erases its NVS; set acknowledge_recovery to true");
		return;
	}
	coprocessor_manager_get_status(&cp);
	if (cp.uart_mode == COPROCESSOR_UART_USB_BRIDGE || cp.uart_mode == COPROCESSOR_UART_FLASHING) {
		web_api_reject(call, API_ERR_BUSY,
			       cp.uart_mode == COPROCESSOR_UART_USB_BRIDGE
				       ? "The coprocessor's UART is bridged to USB"
				       : "The coprocessor's UART is held by a flasher");
		return;
	}
	if (cp.uart_mode != COPROCESSOR_UART_CONSOLE) {
		web_api_reject(call, API_ERR_CAPABILITY_UNAVAILABLE,
			       "The coprocessor's UART cannot be handed to the flasher");
		return;
	}
	rc = coprocessor_updater_check();
	if (rc == -EBUSY) {
		web_api_reject(call, API_ERR_BUSY, "A coprocessor update is already running");
		return;
	}
	if (rc != 0) {
		web_api_reject(call, API_ERR_SERVICE_NOT_READY, "The coprocessor updater is not ready");
		return;
	}

	created = job_create(&params, &job);
	if (created != JOB_CREATE_NEW) {
		reject_job_create(call, created);
		return;
	}
	rc = coprocessor_updater_start(up.id, job.id);
	if (rc != 0) {
		/* Nothing was accepted: the job must not look like an install. */
		(void)job_cancel(job.id);
		if (rc == -EBUSY) {
			web_api_reject(call, API_ERR_BUSY, "A coprocessor update is already running");
		} else {
			web_api_reject(call, API_ERR_INTERNAL_ERROR, "The install could not be recorded");
		}
		return;
	}
	if (v1_worker_submit(&update_work) < 0) {
		LOG_ERR("install %s accepted but not queued", job.id);
		(void)job_fail(job.id, "internal_error", true);
	}
	web_api_reply_accepted(call, job.id, STATUS_URL);
#endif
}
