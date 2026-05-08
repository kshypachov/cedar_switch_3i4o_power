//
// Created by Kirill Shypachov on 06.05.2026.
//
#include <zephyr/kernel.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include "../http_common.h"

#include "config/device-config.h"


LOG_MODULE_REGISTER(REST_API_inputs_state, LOG_LEVEL_DBG);

static int inputs_state_handler(struct http_client_ctx *client,
                                enum http_data_status status,
                                const struct http_request_ctx *request_ctx,
                                struct http_response_ctx *response_ctx,
                                void *user_data) {

    ARG_UNUSED(status);
    ARG_UNUSED(request_ctx);
    ARG_UNUSED(user_data);

    static char resp_buf[128];

    if (client->method == HTTP_GET) {
        LOG_INF("GET /api/inputs/state");

        bool in[3] = {false, false, false};
        for (size_t i = 0; i < accessories_count; i++) {
            enum io_events ev = accessories_list[i].device_event;
            if (ev >= INPUT_1_STATUS && ev <= INPUT_3_STATUS) {
                in[ev - INPUT_1_STATUS] = accessories_list[i].state;
            }
        }

        int n = snprintk(resp_buf, sizeof(resp_buf),
                         "{\"input1\": %s, "
                         "\"input2\": %s, "
                         "\"input3\": %s}",
                         in[0] ? "true" : "false",
                         in[1] ? "true" : "false",
                         in[2] ? "true" : "false");

        if (n < 0 || n >= (int)sizeof(resp_buf)) {
            response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
            static const char err_json[] = "{\"error\":\"format\"}";
            response_ctx->body = (uint8_t *)err_json;
            response_ctx->body_len = sizeof(err_json) - 1;
            response_ctx->final_chunk = true;
            return 0;
        }

        LOG_DBG("GET /api/inputs/state: %s", resp_buf);
        response_ctx->status = HTTP_200_OK;
        response_ctx->body = (uint8_t *)resp_buf;
        response_ctx->body_len = (size_t)n;
        response_ctx->final_chunk = true;
        return 0;

    } else if (client->method == HTTP_OPTIONS) {
        LOG_INF("OPTIONS /api/inputs/state");

        static struct http_header response_headers[] = {
            {.name = "Access-Control-Allow-Origin", .value = "*"},
        };

        response_ctx->status = HTTP_200_OK;
        response_ctx->headers = response_headers;
        response_ctx->header_count = ARRAY_SIZE(response_headers);
        response_ctx->final_chunk = true;
        return 0;

    } else {
        return -1;
    }
}

static struct http_resource_detail_dynamic inputs_state_api = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_OPTIONS),
        .content_type = "application/json"
    },
    .cb = inputs_state_handler,
    .user_data = NULL,
};

HTTP_RESOURCE_DEFINE(api_inputs_state,
                     http_service,
                     "/api/inputs/state",
                     &inputs_state_api);