/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The device's one HTTP service: API v1 and the browser application.
 *
 * What is decided here and why:
 *
 * - **One service on [::]:80.** With CONFIG_NET_IPV4_MAPPING_TO_IPV6 the IPv6
 *   socket also accepts IPv4, so both families reach the interface on the same
 *   port (owner's decision 2026-09-13; P0 found the old service bound to
 *   0.0.0.0 only). The legacy service on 8080, which wrote STM32 images
 *   without authentication, is gone with the rest of the legacy routes.
 *
 * - **One resource per API path.** Zephyr serialises a dynamic resource across
 *   clients: while one client is mid-body on a resource, another client's
 *   request for it is answered with a bare 409 (measured in P2). Separate
 *   resources confine that to requests for the same path. Resource names sort
 *   in match order - the server tries wildcard patterns in section order, which
 *   is name order (measured) - so a more specific pattern must sort first.
 *
 * - **Everything else goes to the fallback**, which answers an unmatched /api/
 *   path with the contract's JSON 404 and every other path from web-assets:
 *   files, SPA navigation, 404, never a redirect.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/random/random.h>

#include <api_validation/api_validation.h>
#include <job_manager/job_manager.h>
#include <web_api/web_api_http.h>
#include <web_assets/web_assets.h>
#include <web_auth/web_auth.h>
#include <web_auth/web_auth_adapters.h>

#include <app_version.h>

#include "../services/system/system_service.h"
#include "api/v1/web_api_v1.h"
#include "cedar_version.h"
#include "web_server.h"

LOG_MODULE_REGISTER(web_server, LOG_LEVEL_INF);

#define MODEL "cedar_switch_3in4out_power"

static uint16_t http_port = 80;

/* -- the browser application -------------------------------------------- */

static void answer_static(struct web_api_context *ctx)
{
	const struct web_assets_request req = {
		.is_get = ctx->req.method == WEB_API_GET,
		.path = ctx->req.path,
		.accept_encoding = ctx->req.headers.accept_encoding,
		.if_none_match = ctx->req.headers.if_none_match,
	};
	struct web_assets_response out;
	size_t n;

	web_assets_respond(&web_assets, &req, &out);

	n = MIN(out.header_count, ARRAY_SIZE(ctx->rsp.headers));
	ctx->rsp.status = out.status;
	ctx->rsp.body = (const char *)out.body;
	ctx->rsp.body_len = out.body_len;
	for (size_t i = 0; i < n; i++) {
		ctx->rsp.headers[i].name = out.headers[i].name;
		ctx->rsp.headers[i].value = out.headers[i].value;
	}
	ctx->rsp.header_count = n;
	ctx->rsp.close_connection = out.close_connection;
}

/* -- resources ----------------------------------------------------------- */

static int web_callback(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *request_ctx,
			struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(user_data);

	return web_api_http_callback(&web_api_v1_router, answer_static, client, status,
				     request_ctx, response_ctx);
}

static struct http_resource_detail_dynamic fallback_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = WEB_API_HTTP_METHODS,
	},
	.cb = web_callback,
};

HTTP_SERVICE_DEFINE(http_service, "::", &http_port, CONFIG_HTTP_SERVER_MAX_CLIENTS, 10, NULL,
		    &fallback_detail.common, NULL);

#define WEB_API_V1_RESOURCE(_name, _pattern)                                                       \
	static struct http_resource_detail_dynamic _name##_detail = {                              \
		.common = {                                                                        \
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,                                        \
			.bitmask_of_supported_http_methods = WEB_API_HTTP_METHODS,                 \
			.content_type = "application/json",                                        \
		},                                                                                 \
		.cb = web_callback,                                                                \
	};                                                                                         \
	HTTP_RESOURCE_DEFINE(_name, http_service, _pattern, &_name##_detail);

#include "api/v1/http_resources.h"

/* -- start ---------------------------------------------------------------- */

static const struct web_auth_platform auth_platform = {
	.random = web_auth_csrand,
	.now_ms = web_auth_uptime_ms,
	.kdf = web_auth_psa_pbkdf2,
	.load_verifier = web_auth_store_load,
	.save_verifier = web_auth_store_save,
};

static char device_id[32];
static char boot_id[24];
static struct web_api_v1_identity identity;

static void hex(char *out, const uint8_t *bytes, size_t n)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[bytes[i] >> 4];
		out[2 * i + 1] = digits[bytes[i] & 0x0F];
	}
	out[2 * n] = '\0';
}

static void make_identity(void)
{
	uint8_t uid[12];
	uint8_t nonce[8];
	ssize_t n = hwinfo_get_device_id(uid, sizeof(uid));

	strcpy(device_id, "cedar-");
	if (n > 0) {
		hex(device_id + 6, uid, (size_t)n);
	} else {
		strcat(device_id, "unknown");
	}

	if (sys_csrand_get(nonce, sizeof(nonce)) != 0) {
		sys_rand_get(nonce, sizeof(nonce));
	}
	strcpy(boot_id, "boot_");
	hex(boot_id + 5, nonce, sizeof(nonce));

	identity = (struct web_api_v1_identity){
		.device_id = device_id,
		.model = MODEL,
		/* The MCUboot image version, major.minor.revision+build, as slot 1's
		 * header says - what the STM32 update compares and shows
		 * (reports/stm32-update); the compiled-in VERSION string only when the
		 * header could not be read. The source revision stays in the start log. */
		.firmware_version = system_service_running_version() != NULL
					    ? system_service_running_version()
					    : APP_VERSION_TWEAK_STRING,
		.frontend_version = web_assets.version,
		.boot_id = boot_id,
	};
}

int app_web_init(void)
{
	struct web_auth_state state;
	int rc;

	make_identity();
	api_request_id_seed(k_cycle_get_32());
	/* job_manager_init() runs in main(), before the network service creates jobs. */

	rc = web_auth_init(&auth_platform);
	if (rc != 0) {
		/* The interface still starts: GET /auth/state and the page explain
		 * what is wrong better than a device that does not answer. */
		LOG_ERR("web sign-in unavailable: %d", rc);
	}
	web_auth_get_state(&state);
	if (state.setup_allowed) {
		LOG_WRN("no administrator yet: setup is open on the web page");
	}

	rc = web_api_v1_init(&identity);
	if (rc != 0) {
		LOG_ERR("API v1 init failed: %d", rc);
	}

	LOG_INF("web interface %s, firmware %s (built as %s, %s), frontend %s, %s", device_id,
		identity.firmware_version, APP_VERSION_TWEAK_STRING, CEDAR_FIRMWARE_VERSION,
		web_assets.version, boot_id);

	return http_server_start();
}
