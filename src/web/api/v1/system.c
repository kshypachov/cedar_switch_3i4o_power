/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1: system status, capabilities and jobs.
 */

#include <string.h>

#include <stdio.h>

#include <zephyr/kernel.h>

#include <device_config_store/device_config_store.h>
#include <matter_service/matter_service.h>

#include "v1_internal.h"

/* -- system status ------------------------------------------------------- */

void v1_get_system_status(struct web_api_call *call)
{
	const struct web_api_v1_identity *id = v1_identity();
	char ids[16][JOB_ID_MAX_LEN + 1];
	/* Not inside MIN(), which would evaluate the call twice. */
	size_t count = job_active_ids(ids, ARRAY_SIZE(ids));
	struct web_json_writer *w;

	count = MIN(count, ARRAY_SIZE(ids));
	w = web_api_json(call);

	web_json_object_begin(w);
	web_json_key(w, "device_id");
	web_json_string(w, id->device_id);
	web_json_key(w, "model");
	web_json_string(w, id->model);
	web_json_key(w, "firmware_version");
	web_json_string(w, id->firmware_version);
	web_json_key(w, "frontend_version");
	web_json_string(w, id->frontend_version);
	web_json_key(w, "boot_id");
	web_json_string(w, id->boot_id);
	web_json_key(w, "uptime_ms");
	web_json_decimal(w, (uint64_t)k_uptime_get());
	web_json_key(w, "wall_time");
	/* No clock source is synchronised on this device yet. */
	web_json_null(w);
	web_json_key(w, "active_job_ids");
	web_json_array_begin(w);
	for (size_t i = 0; i < count; i++) {
		web_json_string(w, ids[i]);
	}
	web_json_array_end(w);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

/* -- capabilities -------------------------------------------------------- */

/*
 * Limits whose owning service arrives in a later stage are the contract's
 * design values, published so a client has a number to respect; each is
 * replaced by the service's own Kconfig when that service exists.
 */
#define UPLOAD_CHUNK_BYTES       16384 /* firmware-store, P6 */
#define UPLOAD_MAX_BYTES         (2 * 1024 * 1024) /* firmware-store, P6 */
#define LOG_PAGE_RECORDS         100   /* log-store, P5 */

#if defined(CONFIG_NETWORK_MANAGER)
#define SCAN_RECORDS         CONFIG_NETWORK_MANAGER_SCAN_MAX_RESULTS
#define CONFIRM_MIN_SECONDS  CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MIN_SECONDS
#define CONFIRM_MAX_SECONDS  CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MAX_SECONDS
#else
#define SCAN_RECORDS         64
#define CONFIRM_MIN_SECONDS  60
#define CONFIRM_MAX_SECONDS  300
#endif

static void feature(struct web_json_writer *w, const char *name, bool available,
		    const char *reason)
{
	web_json_key(w, name);
	web_json_object_begin(w);
	web_json_key(w, "available");
	web_json_bool(w, available);
	web_json_key(w, "reason");
	web_json_string_or_null(w, reason);
	web_json_object_end(w);
}

void v1_get_capabilities(struct web_api_call *call)
{
	struct web_json_writer *w = web_api_json(call);
	struct matter_status matter;
	uint32_t window_min_s;
	uint32_t window_max_s;

	matter_service_window_limits(&window_min_s, &window_max_s);

	web_json_object_begin(w);
	web_json_key(w, "api_version");
	web_json_string(w, "1");

	/* Every feature answers from what this build serves, not from what the
	 * hardware might do. Matter is available once its stack runs; the others
	 * have no API yet (plan section 10). */
	web_json_key(w, "features");
	web_json_object_begin(w);
	matter_service_get_status(&matter);
	feature(w, "matter", matter.state == MATTER_STATE_READY,
		matter.state == MATTER_STATE_READY ? NULL : matter_state_str(matter.state));
	feature(w, "esp32_logs", false, "not_implemented");
	feature(w, "esp32_ota", false, "not_implemented");
	feature(w, "esp32_uart", false, "not_implemented");
	web_json_object_end(w);

	web_json_key(w, "limits");
	web_json_object_begin(w);
	web_json_key(w, "json_body_bytes");
	web_json_int(w, CONFIG_WEB_API_JSON_BODY_MAX);
	web_json_key(w, "upload_chunk_bytes");
	web_json_int(w, UPLOAD_CHUNK_BYTES);
	web_json_key(w, "upload_max_bytes");
	web_json_int(w, UPLOAD_MAX_BYTES);
	web_json_key(w, "log_page_records");
	web_json_int(w, LOG_PAGE_RECORDS);
	web_json_key(w, "scan_records");
	web_json_int(w, SCAN_RECORDS);
	web_json_key(w, "commissioning_min_seconds");
	web_json_int(w, window_min_s);
	web_json_key(w, "commissioning_max_seconds");
	web_json_int(w, window_max_s);
	web_json_key(w, "network_confirm_min_seconds");
	web_json_int(w, CONFIRM_MIN_SECONDS);
	web_json_key(w, "network_confirm_max_seconds");
	web_json_int(w, CONFIRM_MAX_SECONDS);
	web_json_object_end(w);

	/* From the network service: what this build's Wi-Fi driver can join. */
	const uint32_t modes = v1_wifi_security_modes();

	web_json_key(w, "wifi_security_modes");
	web_json_array_begin(w);
	for (int s = 0; s < DEVICE_CONFIG_WIFI_SECURITY_COUNT; s++) {
		if (modes & BIT(s)) {
			web_json_string(w, device_config_wifi_security_str(s));
		}
	}
	web_json_array_end(w);

	web_json_key(w, "firmware_formats");
	web_json_array_begin(w);
	web_json_string(w, "raw_app");
	web_json_array_end(w);

	web_json_key(w, "update_requires_ethernet");
	web_json_bool(w, true);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

/* -- jobs ----------------------------------------------------------------- */

const char *v1_job_resource_url(const struct job_snapshot *job, char *buf, size_t cap)
{
	switch (job->kind) {
	case JOB_KIND_PASSWORD_CHANGE:
		return WEB_API_BASE_PATH "/auth/session";
	case JOB_KIND_MATTER_OPEN:
	case JOB_KIND_MATTER_CLOSE:
		return WEB_API_BASE_PATH "/matter/commissioning";
	case JOB_KIND_NETWORK_APPLY:
	case JOB_KIND_NETWORK_DISCARD:
		return WEB_API_BASE_PATH "/network/config";
	case JOB_KIND_WIFI_SCAN:
		(void)snprintf(buf, cap, WEB_API_BASE_PATH "/network/wifi/scans/%s", job->id);
		return buf;
	default:
		return NULL;
	}
}

static const char *progress_unit(enum job_progress_unit unit)
{
	return unit == JOB_PROGRESS_UNIT_BYTES ? "bytes" : "steps";
}

void v1_get_job(struct web_api_call *call)
{
	const struct web_api_v1_identity *id = v1_identity();
	struct job_snapshot job;
	struct web_json_writer *w;
	char url[96];

	if (job_get(call->params[0], &job) != 0) {
		web_api_reject(call, API_ERR_NOT_FOUND, "No such job");
		return;
	}

	w = web_api_json(call);
	web_json_object_begin(w);
	web_json_key(w, "id");
	web_json_string(w, job.id);
	web_json_key(w, "boot_id");
	web_json_string(w, id->boot_id);
	web_json_key(w, "kind");
	web_json_string(w, job_kind_str(job.kind));
	web_json_key(w, "state");
	web_json_string(w, job_state_str(job.state));
	web_json_key(w, "phase");
	web_json_string(w, job.phase);
	web_json_key(w, "progress");
	if (job.compact || job.progress.unit == JOB_PROGRESS_UNIT_NONE) {
		web_json_null(w);
	} else {
		web_json_object_begin(w);
		web_json_key(w, "completed");
		web_json_int(w, (int64_t)job.progress.completed);
		web_json_key(w, "total");
		if (job.progress.total_known) {
			web_json_int(w, (int64_t)job.progress.total);
		} else {
			web_json_null(w);
		}
		web_json_key(w, "unit");
		web_json_string(w, progress_unit(job.progress.unit));
		web_json_object_end(w);
	}
	web_json_key(w, "cancellable");
	web_json_bool(w, job.cancellable);
	web_json_key(w, "created_uptime_ms");
	web_json_decimal(w, (uint64_t)job.created_uptime_ms);
	web_json_key(w, "updated_uptime_ms");
	web_json_decimal(w, (uint64_t)job.updated_uptime_ms);
	web_json_key(w, "resource_url");
	web_json_string_or_null(w, v1_job_resource_url(&job, url, sizeof(url)));
	web_json_key(w, "error");
	if (job.has_error) {
		web_json_object_begin(w);
		web_json_key(w, "code");
		web_json_string(w, job.error.code);
		web_json_key(w, "message");
		web_json_string(w, "The operation failed");
		web_json_key(w, "request_id");
		web_json_string(w, call->ctx->rsp.request_id);
		web_json_key(w, "retryable");
		web_json_bool(w, job.error.retryable);
		web_json_object_end(w);
	} else {
		web_json_null(w);
	}
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}
