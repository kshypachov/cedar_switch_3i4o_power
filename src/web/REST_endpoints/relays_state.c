//
// Created by Kirill Shypachov on 16.09.2025.
//
#include <zephyr/kernel.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/data/json.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include "../http_common.h"
#include "../../settings_topics.h"
#include "../../zbus_topics.h"

LOG_MODULE_REGISTER(REST_API_relays_state);

typedef struct {
    bool relay1;
    bool relay2;
    bool relay3;
    bool relay4;
} relays_state_t;


void relays_state_update(relays_state_t state) {

    outputs_msg_t relays_state = {0};
    zbus_chan_read(&outputs_zbus_topik, &relays_state, K_NO_WAIT);
    relays_state.seq++;
    relays_state.state = state.relay1 << 0 | state.relay2 << 1 | state.relay3 << 2 | state.relay4 << 3;
    zbus_chan_pub(&outputs_zbus_topik, &relays_state, K_NO_WAIT);

}

int relays_state_get(void) {

    outputs_msg_t relays_state = {0};
    zbus_chan_read(&outputs_zbus_topik, &relays_state, K_NO_WAIT);
    return relays_state.state;
}

static const struct json_obj_descr relays_state_descr[] = {
    JSON_OBJ_DESCR_PRIM_NAMED(relays_state_t, "relay1", relay1, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(relays_state_t, "relay2", relay2, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(relays_state_t, "relay3", relay3, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(relays_state_t, "relay4", relay4, JSON_TOK_TRUE),
};

static int relays_state_handler(struct http_client_ctx *client,
                            enum http_data_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data) {

    ARG_UNUSED(user_data);

    static char resp_buf[256] = "\0";
    static char post_request_buff [256] = "\0";
    static size_t cursor;
    uint8_t relays_state = 0;


    if (client->method == HTTP_GET) {

        LOG_INF("GET /api/relays/state");
        relays_state = relays_state_get();
        /* Формируем JSON строку */
        int n = snprintk(resp_buf, sizeof(resp_buf),
                         "{\"relay1\": %s, "
                         "\"relay2\": %s, "
                         "\"relay3\": %s, "
                         "\"relay4\": %s }",
                         (relays_state & 1) ? "true" : "false" ,
                         (relays_state & 2) ? "true" : "false" ,
                         (relays_state & 4) ? "true" : "false" ,
                         (relays_state & 8) ? "true" : "false" );

        if (n < 0 || n >= (int)sizeof(resp_buf)) {
            response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
            static const char err_json[] = "{\"error\":\"format\"}";
            response_ctx->body = (uint8_t *)err_json;
            response_ctx->body_len = sizeof(err_json) - 1;
            response_ctx->final_chunk = true;

            return 0;
        }
        response_ctx->status = HTTP_200_OK;
        response_ctx->body = (uint8_t *) resp_buf;
        response_ctx->body_len = strlen(resp_buf);
        response_ctx->final_chunk = true;

        return 0;

    }else if (client->method == HTTP_POST) {
        LOG_INF("POST /api/relays/state");

        if (status == HTTP_SERVER_DATA_ABORTED) {
            cursor = 0;
            return 0;
        }

        if (request_ctx->data_len + cursor > sizeof(post_request_buff)) {
            cursor = 0;
            return -ENOMEM;
        }

        memcpy(post_request_buff + cursor, request_ctx->data, request_ctx->data_len);
        cursor += request_ctx->data_len;

        if (status == HTTP_SERVER_DATA_FINAL) {

            relays_state_t tmp = {0};
            const int expected = BIT_MASK(ARRAY_SIZE(relays_state_descr));
            int ret = json_obj_parse(post_request_buff, cursor, relays_state_descr, ARRAY_SIZE(relays_state_descr), &tmp);

            cursor = 0;
            if (ret != expected) {
                /* Можно различать: <0 — ошибка парсера, иначе — битовая маска отсутствующих полей */
                response_ctx->status = HTTP_400_BAD_REQUEST;
                static const char msg[] = "{\"error\":\"invalid json or missing fields\"}";
                response_ctx->body = (uint8_t *)msg;
                response_ctx->body_len = sizeof(msg) - 1;
                response_ctx->final_chunk = true;

                return 0;
            }

            relays_state_update(tmp);
            /* Формируем JSON строку */
            int n = snprintk(resp_buf, sizeof(resp_buf),
                             "{\"relay1\":%s, "
                             "\"relay2\": %s, "
                             "\"relay3\": %s, "
                             "\"relay4\": %s }",
                             tmp.relay1 ? "true" : "false" ,
                             tmp.relay2 ? "true" : "false" ,
                             tmp.relay3 ? "true" : "false" ,
                             tmp.relay4 ? "true" : "false" );


            response_ctx->status = HTTP_200_OK; /* альтернативно: HTTP_STATUS_NO_CONTENT */
            response_ctx->body = (uint8_t *)resp_buf;
            response_ctx->body_len = (size_t)n;
            response_ctx->final_chunk = true;

            return 0;
        }

    }else if (client->method == HTTP_OPTIONS) {

        LOG_INF("OPTIONS /api/relays/state");

        static struct http_header response_headers[] = {
            {.name = "Access-Control-Allow-Origin", .value = "*"}
            // {.name = "Access-Control-Allow-Methods", .value = "GET, POST, OPTIONS"},
            // {.name = "Access-Control-Allow-Headers", .value = "*"},
            // {.name = "Access-Control-Max-Age", .value = "86400"},
        };

        response_ctx->status = HTTP_200_OK;
        response_ctx->headers = response_headers;
        response_ctx->header_count = ARRAY_SIZE(response_headers);
        response_ctx->final_chunk = true;
        return 0;

    }else {

        return -1;
    }
return 0;
}


static struct http_resource_detail_dynamic relays_state = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST) | BIT(HTTP_OPTIONS),
        .content_type = "application/json"
    },
    .cb = relays_state_handler,
    .user_data = NULL,
};

/* === Register path for HTTP service only === */
HTTP_RESOURCE_DEFINE(api_relays_state,
                     http_api_service,
                     "/api/relays/state",
                     &relays_state);