/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1: the coprocessor's status.
 *
 * Contract: CoprocessorStatus in openapi.json, "Сессия и устройство" in
 * api-contract.md. The UART's owner and generation are coprocessor-manager's;
 * the transport and firmware version come from the board's coprocessor
 * service through web_api_v1_set_coprocessor(). Decisions the contract leaves
 * to the device:
 *
 * - **`state`**: `updating` while the UART is `flashing`; `ready` when
 *   ESP-Hosted has a transport; `starting` in the first
 *   COPROCESSOR_START_GRACE_MS after boot, while the driver may still be
 *   bringing it up; `failed` when the chip answers on its UART but has no
 *   transport (board B: an empty flash, only the ROM talking); `offline` when
 *   nothing has come from the chip at all. `recovering` belongs to the updater
 *   (P6).
 * - **`firmware_version`** is the version the C6 reports over ESP-Hosted
 *   ("v3.0.6"), and only while the transport is up; `host_protocol` is
 *   "esp-hosted-mcu-<major>" of that version, otherwise null (api-contract.md,
 *   "Решения P6"). `partition_layout_id` is null until the updater knows the
 *   layout.
 * - **`uart_update`** follows the UART's owner: available while the console
 *   holds it, otherwise `uart_usb_bridge`, `uart_flashing` or
 *   `uart_unavailable`. The C6's state does not enter into it. **`ota`** is
 *   `not_implemented`, outside the first version.
 * - **`last_update`** is null until the updater (P6) records an install.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include <coprocessor_manager/coprocessor_manager.h>
#if defined(CONFIG_COPROCESSOR_UPDATER)
#include <coprocessor_updater/coprocessor_updater.h>
#endif

#include "v1_internal.h"

#define COPROCESSOR_START_GRACE_MS 20000

static const struct web_api_v1_coprocessor *cp_hooks;

void web_api_v1_set_coprocessor(const struct web_api_v1_coprocessor *coprocessor)
{
	cp_hooks = coprocessor;
}

static bool firmware_version(char *buf, size_t cap)
{
	return cp_hooks != NULL && cp_hooks->firmware_version != NULL &&
	       cp_hooks->firmware_version(buf, cap);
}

static bool rx_seen(void)
{
	return cp_hooks != NULL && cp_hooks->rx_seen != NULL && cp_hooks->rx_seen();
}

bool v1_request_over_ethernet(const struct web_api_request *req)
{
	return cp_hooks != NULL && cp_hooks->request_over_ethernet != NULL &&
	       cp_hooks->request_over_ethernet(&req->local);
}

/* An install is running: from its acceptance, before it has the UART, to its end. */
static bool updater_active(void)
{
#if defined(CONFIG_COPROCESSOR_UPDATER)
	struct coprocessor_updater_state st;

	coprocessor_updater_get_state(&st);
	return st.active;
#else
	return false;
#endif
}

static const char *state_of(const struct coprocessor_status *s)
{
	if (s->uart_mode == COPROCESSOR_UART_FLASHING || updater_active()) {
		return "updating";
	}
	if (s->transport_ready) {
		return "ready";
	}
	if (k_uptime_get() < COPROCESSOR_START_GRACE_MS) {
		return "starting";
	}

	return rx_seen() ? "failed" : "offline";
}

static void availability(struct web_json_writer *w, const char *name, const char *reason)
{
	web_json_key(w, name);
	web_json_object_begin(w);
	web_json_key(w, "available");
	web_json_bool(w, reason == NULL);
	web_json_key(w, "reason");
	web_json_string_or_null(w, reason);
	web_json_object_end(w);
}

static const char *uart_update_reason(enum coprocessor_uart_mode mode)
{
	/* An accepted install owns the UART already, even before it switches. */
	if (updater_active()) {
		return "uart_flashing";
	}
	/* The same owners, and the same words, as the ESP32 log's reason. */
	return coprocessor_logs_unavailable_reason(mode);
}

const char *v1_uart_update_unavailable_reason(void)
{
	struct coprocessor_status s;

	coprocessor_manager_get_status(&s);
	return uart_update_reason(s.uart_mode);
}

/* UpdateSummary of the last install, or null. */
static void write_last_update(struct web_api_call *call, struct web_json_writer *w)
{
	web_json_key(w, "last_update");
#if defined(CONFIG_COPROCESSOR_UPDATER)
	struct coprocessor_updater_state st;

	coprocessor_updater_get_state(&st);
	if (st.has_last) {
		const struct coprocessor_update_summary *l = &st.last;

		web_json_object_begin(w);
		web_json_key(w, "job_id");
		web_json_string(w, l->job_id);
		web_json_key(w, "state");
		web_json_string(w, coprocessor_update_outcome_str(l->state));
		web_json_key(w, "method");
		web_json_string(w, "uart");
		web_json_key(w, "version");
		web_json_string_or_null(w, l->version[0] != '\0' ? l->version : NULL);
		web_json_key(w, "recovery_required");
		web_json_bool(w, l->recovery_required);
		web_json_key(w, "error");
		if (l->has_error) {
			web_json_object_begin(w);
			web_json_key(w, "code");
			web_json_string(w, l->error_code);
			web_json_key(w, "message");
			web_json_string(w, l->error_message);
			web_json_key(w, "request_id");
			web_json_string(w, call->ctx->rsp.request_id);
			web_json_key(w, "retryable");
			web_json_bool(w, l->error_retryable);
			web_json_object_end(w);
		} else {
			web_json_null(w);
		}
		web_json_object_end(w);
		return;
	}
#else
	ARG_UNUSED(call);
#endif
	web_json_null(w);
}

/* "esp-hosted-mcu-<major>" of a version such as "v3.0.6"; false when it has no major. */
static bool host_protocol_of(const char *version, char *buf, size_t cap)
{
	const char *p = version;
	size_t n = 0;

	if (*p == 'v' || *p == 'V') {
		p++;
	}
	while (p[n] >= '0' && p[n] <= '9' && n < 5U) {
		n++;
	}
	if (n == 0U) {
		return false;
	}
	return snprintf(buf, cap, "esp-hosted-mcu-%.*s", (int)n, p) < (int)cap;
}

void v1_get_coprocessor_status(struct web_api_call *call)
{
	struct coprocessor_status s;
	struct web_json_writer *w;
	/* Empty unless the hook wrote it: never parse what nobody wrote. */
	char version[24] = "";
	char protocol[32];
	bool known;
	bool protocol_known;

	coprocessor_manager_get_status(&s);
	known = s.transport_ready && firmware_version(version, sizeof(version));
	protocol_known = known && host_protocol_of(version, protocol, sizeof(protocol));

	w = web_api_json(call);
	web_json_object_begin(w);
	web_json_key(w, "state");
	web_json_string(w, state_of(&s));
	web_json_key(w, "chip");
	web_json_string(w, "esp32c6");
	web_json_key(w, "firmware_version");
	web_json_string_or_null(w, known ? version : NULL);
	web_json_key(w, "host_protocol");
	web_json_string_or_null(w, protocol_known ? protocol : NULL);
	web_json_key(w, "partition_layout_id");
	web_json_null(w);
	web_json_key(w, "transport_ready");
	web_json_bool(w, s.transport_ready);
	web_json_key(w, "uart_mode");
	web_json_string(w, coprocessor_uart_mode_str(s.uart_mode));
	web_json_key(w, "generation");
	web_json_int(w, s.generation);
	availability(w, "ota", "not_implemented");
	availability(w, "uart_update", uart_update_reason(s.uart_mode));
	write_last_update(call, w);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}
