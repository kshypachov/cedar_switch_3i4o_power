//
// Created by Kirill Shypachov on 14.09.2025.
//
#include "http_server_init.h"
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/logging/log.h>
#include <autoconf.h>
#include "authentication.h"

static uint16_t http_static_service_port = 80;
// static uint16_t http_api_service_port = 8080;
//
LOG_MODULE_REGISTER(http_server_init);
//
// HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_user_agent, "User-Agent");
// HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_authorization, "Authorization");
// HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_cookie, "Cookie");
//
// Fallback handler: Redirect on /
static int fallback_redirect_handler(struct http_client_ctx *client,
                                     enum http_data_status status,
                                     const struct http_request_ctx *request_ctx,
                                     struct http_response_ctx *response_ctx,
                                     void *user_data)
{
    ARG_UNUSED(client);
    ARG_UNUSED(status);
    ARG_UNUSED(request_ctx);
    ARG_UNUSED(user_data);

    static const struct http_header headers[] = {
        { .name = "Location", .value = "/" },
        { .name = "Cache-Control", .value = "no-store" },
    };
// TODO change to 301
    response_ctx->status = HTTP_302_FOUND;   // или HTTP_307_TEMP_REDIRECT
    response_ctx->headers = headers;
    response_ctx->header_count = ARRAY_SIZE(headers);
    response_ctx->body = NULL;
    response_ctx->body_len = 0;
    response_ctx->final_chunk = true;

    return 0;
}

static struct http_resource_detail_dynamic fallback_redirect_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods =
            BIT(HTTP_GET) | BIT(HTTP_HEAD) | BIT(HTTP_POST) | BIT(HTTP_PUT) |
            BIT(HTTP_DELETE) | BIT(HTTP_OPTIONS),
    },
    .cb = fallback_redirect_handler,
    .user_data = NULL,
};

HTTP_SERVICE_DEFINE(http_service, "0.0.0.0", &http_static_service_port, 4, 10, NULL, &fallback_redirect_detail.common, NULL);
//
// HTTP_SERVICE_DEFINE(http_api_service, "0.0.0.0", &http_api_service_port, 1, 10, NULL, NULL, NULL);

void app_http_server_init(void) {
    //authentication_init();
    http_server_start();
}