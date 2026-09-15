/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1: the STM32 firmware update - getSystemFirmware and startSystemUpdate.
 * The upload itself (target stm32u585) goes through firmware.c, which dispatches
 * to system-image-store.
 *
 * Contract: "Обновление STM32" in api-contract.md, SystemFirmware,
 * SystemUpdateRequest and SystemUpdateSummary in openapi.json. The decisions are
 * system-updater's (modules/system-updater/README.md); what the binding decides:
 *
 * - **Refusals in the contract's order**, all before a job exists: unknown
 *   upload 404 (an ESP32 upload 422 unsupported_target); an install of either
 *   processor running or a network transaction applying/awaiting confirmation
 *   409 busy; the running firmware unconfirmed or a swap pending 409
 *   invalid_state; the upload not ready 409 invalid_state; a downgrade without
 *   acknowledge_downgrade 422. The downgrade is checked here against the
 *   updater's snapshot, so a refused request leaves no cancelled job under its
 *   Idempotency-Key; system_updater_start() checks it again.
 * - **A replay by key is answered first**, with the install's job, as the
 *   coprocessor install is.
 * - **The install runs on the v1 worker**, the one thread of system-image-store's
 *   flash I/O. It requests the swap and restarts the device; it never confirms -
 *   confirmation writes slot 1 through the XIP driver and runs on the board
 *   service's thread, whose stack is in SRAM (the worker's is in PSRAM).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "v1_internal.h"

#if defined(CONFIG_SYSTEM_IMAGE_STORE) && defined(CONFIG_SYSTEM_UPDATER)
#include <system_image_store/system_image_store.h>
#include <system_updater/system_updater.h>
#define HAVE_SYSTEM_UPDATE 1
#else
#define HAVE_SYSTEM_UPDATE 0
#endif
#if defined(CONFIG_COPROCESSOR_UPDATER)
#include <coprocessor_updater/coprocessor_updater.h>
#endif
#if defined(CONFIG_NETWORK_MANAGER)
#include <network_manager/network_manager.h>
#endif

LOG_MODULE_REGISTER(web_api_v1_system_update, LOG_LEVEL_INF);

#define FIRMWARE_URL WEB_API_BASE_PATH "/system/firmware"

/* The slot on this board: 4 MiB less MCUboot's 64 KiB trailer sector. Only what
 * capabilities publish before the board opened the store. */
#define SYSTEM_UPLOAD_MAX_DEFAULT (4U * 1024U * 1024U - 64U * 1024U)

static const struct web_api_v1_system *sys_hooks;

void web_api_v1_set_system(const struct web_api_v1_system *system)
{
	sys_hooks = system;
}

const struct web_api_v1_system *v1_system(void)
{
	return sys_hooks;
}

uint32_t v1_system_upload_max_bytes(void)
{
	return (sys_hooks != NULL && sys_hooks->upload_max_bytes > 0U) ? sys_hooks->upload_max_bytes
								       : SYSTEM_UPLOAD_MAX_DEFAULT;
}

/* -- request schema ---------------------------------------------------------- */

static const struct web_json_field system_update_fields[] = {
	{
		.name = "upload_id",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_system_update_body, upload_id),
		.size = sizeof(((struct v1_system_update_body *)0)->upload_id),
		.min_len = 1,
		.max_len = 64,
		.format = api_validate_opaque_id,
	},
	{
		.name = "acknowledge_downgrade",
		.type = WEB_JSON_BOOL,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_system_update_body, acknowledge_downgrade),
	},
};

const struct web_json_object v1_system_update_schema = {
	.fields = system_update_fields,
	.field_count = ARRAY_SIZE(system_update_fields),
};

/* -- what else is going on ------------------------------------------------------ */

bool v1_install_running(void)
{
#if HAVE_SYSTEM_UPDATE
	/* Only the HTTP thread reads these snapshots: static rather than on its stack. */
	static struct system_updater_state s;

	system_updater_get_state(&s);
	if (s.active) {
		return true;
	}
#endif
#if defined(CONFIG_COPROCESSOR_UPDATER)
	static struct coprocessor_updater_state cp;

	coprocessor_updater_get_state(&cp);
	if (cp.active) {
		return true;
	}
#endif
	return false;
}

const char *v1_system_update_unavailable_reason(void)
{
#if !HAVE_SYSTEM_UPDATE
	return "not_implemented";
#else
	int rc;

	if (sys_hooks == NULL) {
		return "service_not_ready";
	}
	if (v1_install_running()) {
		return "update_running";
	}
	rc = system_updater_check();
	if (rc == -EACCES) {
		return "firmware_unconfirmed";
	}
	if (rc == -EBUSY) {
		return "update_running";
	}
	return rc == 0 ? NULL : "service_not_ready";
#endif
}

#if HAVE_SYSTEM_UPDATE

static int64_t now_ms(void)
{
	return (sys_hooks != NULL && sys_hooks->now_ms != NULL) ? sys_hooks->now_ms() : k_uptime_get();
}

/* A restart in the middle of an apply turns it into a rollback (410 boot_changed). */
static bool network_changing(void)
{
#if defined(CONFIG_NETWORK_MANAGER)
	static struct network_transaction txn;

	if (network_transaction_current(&txn) == 0 &&
	    (txn.state == NETWORK_TXN_APPLYING || txn.state == NETWORK_TXN_AWAITING_CONFIRMATION)) {
		return true;
	}
#endif
	return false;
}

/* -- responses ------------------------------------------------------------------ */

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

static void write_version_or_null(struct web_json_writer *w, bool has,
				  const struct system_image_version *v)
{
	char text[SYSTEM_UPDATE_VERSION_STR_LEN];

	if (has && system_image_version_str(v, text, sizeof(text)) >= 0) {
		web_json_string(w, text);
	} else {
		web_json_null(w);
	}
}

static void write_summary(struct web_api_call *call, struct web_json_writer *w,
			  const struct system_update_summary *last)
{
	web_json_object_begin(w);
	web_json_key(w, "job_id");
	web_json_string(w, last->job_id);
	web_json_key(w, "state");
	web_json_string(w, system_update_outcome_str(last->state));
	web_json_key(w, "from_version");
	write_version_or_null(w, last->has_from_version, &last->from_version);
	web_json_key(w, "version");
	write_version_or_null(w, last->has_version, &last->version);
	web_json_key(w, "error");
	if (last->has_error) {
		web_json_object_begin(w);
		web_json_key(w, "code");
		web_json_string(w, last->error_code);
		web_json_key(w, "message");
		web_json_string(w, last->error_message[0] != '\0' ? last->error_message
								  : "The update did not complete");
		web_json_key(w, "request_id");
		web_json_string(w, call->ctx->rsp.request_id);
		web_json_key(w, "retryable");
		web_json_bool(w, last->error_retryable);
		web_json_object_end(w);
	} else {
		web_json_null(w);
	}
	web_json_object_end(w);
}

void v1_get_system_firmware(struct web_api_call *call)
{
	static struct system_updater_state s;
	struct web_json_writer *w;
	const char *reason;
	char text[SYSTEM_UPDATE_VERSION_STR_LEN];
	char digest[65];

	if (sys_hooks == NULL) {
		web_api_reject(call, API_ERR_SERVICE_NOT_READY, "The STM32 updater is not ready");
		return;
	}
	reason = v1_system_update_unavailable_reason();
	system_updater_get_state(&s);

	w = web_api_json(call);
	web_json_object_begin(w);
	web_json_key(w, "running");
	web_json_object_begin(w);
	web_json_key(w, "version");
	if (s.running_known && system_image_version_str(&s.running.version, text, sizeof(text)) >= 0) {
		web_json_string(w, text);
	} else {
		web_json_string(w, v1_identity()->firmware_version);
	}
	web_json_key(w, "image_hash");
	if (s.running_known) {
		web_json_string(w, hex(s.running.hash, sizeof(s.running.hash), digest));
	} else {
		web_json_null(w);
	}
	web_json_key(w, "confirmed");
	web_json_bool(w, s.running_known && s.running_confirmed);
	web_json_object_end(w);

	web_json_key(w, "confirm_remaining_seconds");
	if (s.confirm_pending) {
		web_json_int(w, s.confirm_remaining_seconds);
	} else {
		web_json_null(w);
	}
	web_json_key(w, "swap_pending");
	web_json_bool(w, s.swap_pending);
	web_json_key(w, "update");
	web_json_object_begin(w);
	web_json_key(w, "available");
	web_json_bool(w, reason == NULL);
	web_json_key(w, "reason");
	web_json_string_or_null(w, reason);
	web_json_object_end(w);
	web_json_key(w, "last_update");
	if (s.has_last) {
		write_summary(call, w, &s.last);
	} else {
		web_json_null(w);
	}
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

/* -- startSystemUpdate ---------------------------------------------------------- */

static void run_system_update(struct k_work *work)
{
	ARG_UNUSED(work);
	system_updater_run();
}

static K_WORK_DEFINE(system_update_work, run_system_update);

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

static const char MSG_UNCONFIRMED[] =
	"The running firmware is not confirmed yet: slot 2 holds the firmware a revert would "
	"restore. Wait for the confirmation or run `sysupd confirm` on the console";
static const char MSG_DOWNGRADE[] =
	"The image is older than the running firmware; set acknowledge_downgrade to true";

void v1_start_system_update(struct web_api_call *call)
{
	const struct v1_system_update_body *body = call->body;
	const struct job_create_params params = {
		.kind = JOB_KIND_SYSTEM_UPDATE,
		/* The updater turns this off at `requesting`. */
		.cancellable = true,
		.idempotency_key = call->scoped_key,
		.request_hash = call->request_hash,
	};
	static struct sys_img_upload up;
	static struct system_updater_state s;
	struct sys_img_image img;
	struct job_snapshot job;
	enum job_create_result created;
	int rc;

	/* A retry of an accepted install is its job, whatever changed since. */
	switch (job_find_by_key(call->scoped_key, call->request_hash, &job)) {
	case JOB_LOOKUP_EXISTING:
		web_api_reply_accepted(call, job.id, FIRMWARE_URL);
		return;
	case JOB_LOOKUP_CONFLICT:
		web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
			       "This Idempotency-Key was used with a different request");
		return;
	default:
		break;
	}
	if (sys_hooks == NULL) {
		web_api_reject(call, API_ERR_SERVICE_NOT_READY, "The STM32 updater is not ready");
		return;
	}

	if (sys_img_get(body->upload_id, now_ms(), &up) != 0) {
		if (strncmp(body->upload_id, V1_SYSTEM_UPLOAD_PREFIX,
			    strlen(V1_SYSTEM_UPLOAD_PREFIX)) != 0 &&
		    strncmp(body->upload_id, "upload_", strlen("upload_")) == 0) {
			/* Not proof the ESP32 upload exists; firmware.c answers for it. The
			 * shape of the id is enough to say it is not an STM32 image. */
			web_api_reject(call, API_ERR_UNSUPPORTED_TARGET,
				       "This upload is an ESP32 image; install it through "
				       "/coprocessor/updates");
		} else {
			web_api_reject(call, API_ERR_NOT_FOUND, "No such upload");
		}
		return;
	}
	if (v1_install_running() || network_changing()) {
		web_api_reject(call, API_ERR_BUSY,
			       "An update is already running, or a network change is being applied");
		return;
	}
	rc = system_updater_check();
	if (rc == -EBUSY) {
		web_api_reject(call, API_ERR_BUSY, "An STM32 update is already running");
		return;
	}
	if (rc == -EACCES) {
		web_api_reject(call, API_ERR_INVALID_STATE, MSG_UNCONFIRMED);
		return;
	}
	if (rc != 0) {
		web_api_reject(call, API_ERR_SERVICE_NOT_READY, "The STM32 updater is not ready");
		return;
	}
	if (up.state != SYS_IMG_READY || sys_img_image_info(up.id, &img) != 0) {
		web_api_reject(call, API_ERR_INVALID_STATE, "The upload is not verified and ready");
		return;
	}
	system_updater_get_state(&s);
	if (s.running_known && !body->acknowledge_downgrade) {
		const struct system_image_version staged = {
			.major = img.major,
			.minor = img.minor,
			.revision = img.revision,
			.build = img.build,
		};

		if (system_image_version_cmp(&staged, &s.running.version) < 0) {
			web_api_reject(call, API_ERR_VALIDATION_FAILED, MSG_DOWNGRADE);
			return;
		}
	}

	created = job_create(&params, &job);
	if (created != JOB_CREATE_NEW) {
		reject_job_create(call, created);
		return;
	}
	rc = system_updater_start(up.id, job.id, body->acknowledge_downgrade);
	if (rc != 0) {
		/* Nothing was accepted: the job must not look like an install. */
		(void)job_cancel(job.id);
		switch (rc) {
		case -EBUSY:
			web_api_reject(call, API_ERR_BUSY, "An STM32 update is already running");
			break;
		case -EACCES:
			web_api_reject(call, API_ERR_INVALID_STATE, MSG_UNCONFIRMED);
			break;
		case -ENOENT:
			web_api_reject(call, API_ERR_NOT_FOUND, "No such upload");
			break;
		case -ENODATA:
			web_api_reject(call, API_ERR_INVALID_STATE,
				       "The upload is not verified and ready");
			break;
		case -EDOM:
			web_api_reject(call, API_ERR_VALIDATION_FAILED, MSG_DOWNGRADE);
			break;
		default:
			web_api_reject(call, API_ERR_INTERNAL_ERROR, "The install could not be recorded");
			break;
		}
		return;
	}
	if (v1_worker_submit(&system_update_work) < 0) {
		LOG_ERR("install %s accepted but not queued", job.id);
		(void)job_fail(job.id, "internal_error", true);
	}
	web_api_reply_accepted(call, job.id, FIRMWARE_URL);
}

#else /* !HAVE_SYSTEM_UPDATE */

void v1_get_system_firmware(struct web_api_call *call)
{
	web_api_reject(call, API_ERR_CAPABILITY_UNAVAILABLE, "This build has no STM32 updater");
}

void v1_start_system_update(struct web_api_call *call)
{
	struct job_snapshot job;

	switch (job_find_by_key(call->scoped_key, call->request_hash, &job)) {
	case JOB_LOOKUP_EXISTING:
		web_api_reply_accepted(call, job.id, FIRMWARE_URL);
		return;
	case JOB_LOOKUP_CONFLICT:
		web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
			       "This Idempotency-Key was used with a different request");
		return;
	default:
		break;
	}
	web_api_reject(call, API_ERR_CAPABILITY_UNAVAILABLE, "This build has no STM32 updater");
}

#endif /* HAVE_SYSTEM_UPDATE */
