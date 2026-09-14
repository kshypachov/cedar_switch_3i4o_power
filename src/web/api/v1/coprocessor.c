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
 * - **`firmware_version`** is the version ESP-Hosted reported, and only while
 *   the transport is up; `host_protocol` is "esp-hosted-mcu" then, otherwise
 *   null. `partition_layout_id` is null until the updater knows the layout.
 * - **`uart_update`** and **`ota`** are `not_implemented` in P5: the UART
 *   updater is P6, OTA is outside the first version.
 */

#include <zephyr/kernel.h>

#include <coprocessor_manager/coprocessor_manager.h>

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

static const char *state_of(const struct coprocessor_status *s)
{
	if (s->uart_mode == COPROCESSOR_UART_FLASHING) {
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

static void unavailable(struct web_json_writer *w, const char *name)
{
	web_json_key(w, name);
	web_json_object_begin(w);
	web_json_key(w, "available");
	web_json_bool(w, false);
	web_json_key(w, "reason");
	web_json_string(w, "not_implemented");
	web_json_object_end(w);
}

void v1_get_coprocessor_status(struct web_api_call *call)
{
	struct coprocessor_status s;
	struct web_json_writer *w;
	char version[24];
	bool known;

	coprocessor_manager_get_status(&s);
	known = s.transport_ready && firmware_version(version, sizeof(version));

	w = web_api_json(call);
	web_json_object_begin(w);
	web_json_key(w, "state");
	web_json_string(w, state_of(&s));
	web_json_key(w, "chip");
	web_json_string(w, "esp32c6");
	web_json_key(w, "firmware_version");
	web_json_string_or_null(w, known ? version : NULL);
	web_json_key(w, "host_protocol");
	web_json_string_or_null(w, s.transport_ready ? "esp-hosted-mcu" : NULL);
	web_json_key(w, "partition_layout_id");
	web_json_null(w);
	web_json_key(w, "transport_ready");
	web_json_bool(w, s.transport_ready);
	web_json_key(w, "uart_mode");
	web_json_string(w, coprocessor_uart_mode_str(s.uart_mode));
	web_json_key(w, "generation");
	web_json_int(w, s.generation);
	unavailable(w, "ota");
	unavailable(w, "uart_update");
	web_json_key(w, "last_update");
	web_json_null(w);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}
