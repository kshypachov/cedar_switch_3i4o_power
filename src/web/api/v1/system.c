/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1: system status, capabilities and jobs.
 */

#include <errno.h>
#include <string.h>

#include <stdio.h>

#include <zephyr/fatal_types.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

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

/* -- coredump ------------------------------------------------------------ */

/*
 * The board's Zephyr coredump (DEBUG_COREDUMP_BACKEND_FLASH_PARTITION on the
 * SPI NOR), behind hooks so the bindings run on native_sim, which has no
 * coredump support. The dump is Zephyr's binary format: scripts/coredump/
 * coredump_gdbserver.py serves it to GDB together with the image's ELF.
 */

static const struct web_api_v1_coredump *dump_hooks;

void web_api_v1_set_coredump(const struct web_api_v1_coredump *coredump)
{
	dump_hooks = coredump;
}

/* struct coredump_hdr_t of zephyr/include/zephyr/debug/coredump.h (packed):
 * "ZE", header version, target, pointer size, flags, then the reason. */
#define DUMP_HDR_LEN        12
#define DUMP_HDR_REASON_OFF 8

static const char *dump_reason_name(uint32_t reason)
{
	switch (reason) {
	case K_ERR_CPU_EXCEPTION:
		return "cpu_exception";
	case K_ERR_SPURIOUS_IRQ:
		return "spurious_irq";
	case K_ERR_STACK_CHK_FAIL:
		return "stack_check_fail";
	case K_ERR_KERNEL_OOPS:
		return "kernel_oops";
	case K_ERR_KERNEL_PANIC:
		return "kernel_panic";
	default:
		/* Architecture codes (K_ERR_ARCH_START and up) are CPU faults too;
		 * reason_code keeps the detail. */
		return reason >= K_ERR_ARCH_START ? "cpu_exception" : "other";
	}
}

static bool dump_ready(struct web_api_call *call)
{
	if (dump_hooks == NULL) {
		web_api_reject(call, API_ERR_CAPABILITY_UNAVAILABLE,
			       "This build does not keep a coredump");
		return false;
	}
	return true;
}

void v1_get_coredump(struct web_api_call *call)
{
	uint8_t hdr[DUMP_HDR_LEN];
	struct web_json_writer *w;
	int size;

	if (!dump_ready(call)) {
		return;
	}
	size = dump_hooks->size();
	if (size < 0) {
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The coredump storage is unreadable");
		return;
	}

	w = web_api_json(call);
	web_json_object_begin(w);
	web_json_key(w, "coredump");
	if (size == 0) {
		web_json_null(w);
	} else {
		bool header = size >= DUMP_HDR_LEN &&
			      dump_hooks->read(0, hdr, sizeof(hdr)) == DUMP_HDR_LEN &&
			      hdr[0] == 'Z' && hdr[1] == 'E';
		uint32_t reason = header ? sys_get_le32(&hdr[DUMP_HDR_REASON_OFF]) : 0U;

		web_json_object_begin(w);
		web_json_key(w, "size_bytes");
		web_json_int(w, size);
		web_json_key(w, "reason");
		web_json_string_or_null(w, header ? dump_reason_name(reason) : NULL);
		web_json_key(w, "reason_code");
		if (header) {
			web_json_int(w, reason);
		} else {
			web_json_null(w);
		}
		web_json_object_end(w);
	}
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

struct dump_stream {
	size_t offset;
	size_t size;
};

BUILD_ASSERT(sizeof(struct dump_stream) <= CONFIG_WEB_API_STREAM_STATE_MAX,
	     "the coredump stream's state must fit CONFIG_WEB_API_STREAM_STATE_MAX");

static int dump_next(void *state, char *buf, size_t cap)
{
	struct dump_stream *st = state;
	size_t want = MIN(cap, st->size - st->offset);
	int n;

	if (want == 0U) {
		return 0;
	}
	n = dump_hooks->read(st->offset, (uint8_t *)buf, want);
	if (n <= 0) {
		/* Closing mid-body tells the client the file is incomplete. */
		return n < 0 ? n : -EIO;
	}
	st->offset += (size_t)n;
	return n;
}

void v1_download_coredump(struct web_api_call *call)
{
	struct dump_stream *st;
	int size;

	if (!dump_ready(call)) {
		return;
	}
	size = dump_hooks->size();
	if (size < 0) {
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The coredump storage is unreadable");
		return;
	}
	if (size == 0) {
		web_api_reject(call, API_ERR_NOT_FOUND, "No coredump is stored");
		return;
	}
	st = web_api_reply_stream(call, 200, "application/octet-stream", sizeof(*st), dump_next,
				  NULL);
	if (st == NULL) {
		return;
	}
	web_api_add_header(call, "Content-Disposition",
			   "attachment; filename=\"cedar-coredump.bin\"");
	st->offset = 0U;
	st->size = (size_t)size;
}

void v1_clear_coredump(struct web_api_call *call)
{
	if (!dump_ready(call)) {
		return;
	}
	if (dump_hooks->clear() != 0) {
		web_api_reject(call, API_ERR_INTERNAL_ERROR, "The coredump could not be cleared");
		return;
	}
	web_api_reply_empty(call, 204);
}

/* -- capabilities -------------------------------------------------------- */

/* A chunk must fit both the request body web-api keeps and firmware-store's
 * staging buffer. */
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
/* ...and system-image-store's, which takes the same chunks for the STM32 image. */
#define UPLOAD_CHUNK_BYTES                                                                         \
	MIN(MIN(CONFIG_WEB_API_OCTET_BODY_MAX, CONFIG_FIRMWARE_STORE_CHUNK_MAX),                   \
	    CONFIG_SYSTEM_IMAGE_STORE_CHUNK_MAX)
#else
#define UPLOAD_CHUNK_BYTES       MIN(CONFIG_WEB_API_OCTET_BODY_MAX, CONFIG_FIRMWARE_STORE_CHUNK_MAX)
#endif
/* The merged coprocessor file is written from 0x0 and must end before ota_1 at
 * 0x1d0000 (api-contract.md, "Формат файла", P6). */
#define UPLOAD_MAX_BYTES         CONFIG_FIRMWARE_STORE_MAX_BYTES
/* The most a page returns (logs.c). A page may return fewer when the response
 * buffer or the scan budget runs out first, and says so with has_more. */
#define LOG_PAGE_RECORDS         100

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
	 * hardware might do. Matter is available once its stack runs. ESP32 logs
	 * follow the UART's owner, not ESP-Hosted: a C6 with no firmware still
	 * prints its ROM, and a working transport says nothing about who reads
	 * the UART. The UART updater follows the UART's owner too, never the C6's
	 * state (an install is how an empty C6 gets firmware); OTA is outside the
	 * first version. */
	web_json_key(w, "features");
	web_json_object_begin(w);
	matter_service_get_status(&matter);
	feature(w, "matter", matter.state == MATTER_STATE_READY,
		matter.state == MATTER_STATE_READY ? NULL : matter_state_str(matter.state));
	const char *esp32_logs_reason = v1_esp32_logs_unavailable_reason(NULL);
	const char *esp32_uart_reason = v1_uart_update_unavailable_reason();

	feature(w, "esp32_logs", esp32_logs_reason == NULL, esp32_logs_reason);
	feature(w, "esp32_ota", false, "not_implemented");
	feature(w, "esp32_uart", esp32_uart_reason == NULL, esp32_uart_reason);
	/* The STM32 update: not while the running image waits for its confirmation
	 * or an install of either processor runs (system_update.c). */
	const char *stm32_update_reason = v1_system_update_unavailable_reason();

	feature(w, "stm32_update", stm32_update_reason == NULL, stm32_update_reason);
	web_json_object_end(w);

	web_json_key(w, "limits");
	web_json_object_begin(w);
	web_json_key(w, "json_body_bytes");
	web_json_int(w, CONFIG_WEB_API_JSON_BODY_MAX);
	web_json_key(w, "upload_chunk_bytes");
	web_json_int(w, UPLOAD_CHUNK_BYTES);
	web_json_key(w, "upload_max_bytes");
	web_json_int(w, UPLOAD_MAX_BYTES);
	web_json_key(w, "system_upload_max_bytes");
	web_json_int(w, v1_system_upload_max_bytes());
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

	/* The only format of the first version (owner's decision, P6): the merged
	 * file that replaces the C6's bootloader, table and application at once. */
	web_json_key(w, "firmware_formats");
	web_json_array_begin(w);
	web_json_string(w, "raw_full_flash");
#if defined(CONFIG_SYSTEM_IMAGE_STORE)
	/* The STM32 application as imgtool signs it (reports/stm32-update). */
	web_json_string(w, "mcuboot_image");
#endif
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
	case JOB_KIND_UPLOAD_CHUNK:
	case JOB_KIND_FIRMWARE_VERIFY:
		return v1_upload_job_resource_url(job, buf, cap);
	case JOB_KIND_COPROCESSOR_UPDATE:
		return WEB_API_BASE_PATH "/coprocessor/status";
	case JOB_KIND_SYSTEM_UPDATE:
		return WEB_API_BASE_PATH "/system/firmware";
	default:
		/* A delete's upload is gone once it succeeds, as the mock answers. */
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
