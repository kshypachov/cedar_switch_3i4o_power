//
// Created by Kirill Shypachov on 16.09.2025.
//
#include <zephyr/kernel.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/sys/printk.h>
#include <zephyr/data/json.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include "../http_common.h"
#include "../../settings_topics.h"
#include "../../zbus_topics.h"
#include <global_var.h>

#include "config/device-config.h"


LOG_MODULE_REGISTER(REST_API_relays_state, LOG_LEVEL_DBG);

/* Local struct used only for JSON parsing of the relay state endpoint */
typedef struct {
    bool relay1;
    bool relay2;
    bool relay3;
    bool relay4;
} relays_state_t;

static void relays_state_update(relays_state_t state) {

    io_event_data_t event;
    bool prev_state[] = {state.relay1, state.relay2, state.relay3, state.relay4};

    event.event_direction = TO_INTERFACE;

    for (size_t i = OUTPUT_1_CONTROL; i <= OUTPUT_4_CONTROL; i++)
    {
        event.io_event = i;
        event.bool_data = prev_state[i - OUTPUT_1_CONTROL];
        zbus_chan_pub(&io_events, &event, K_NO_WAIT);
    }

    event.event_direction = FROM_INTERFACE;

    for (size_t i = OUTPUT_1_CONTROL; i <= OUTPUT_4_CONTROL; i++)
    {
        event.io_event = i;
        event.bool_data = prev_state[i - OUTPUT_1_CONTROL];
        zbus_chan_pub(&io_events, &event, K_NO_WAIT);
    }
}

static const struct json_obj_descr relays_state_descr[] = {
    JSON_OBJ_DESCR_PRIM_NAMED(relays_state_t, "relay1", relay1, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(relays_state_t, "relay2", relay2, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(relays_state_t, "relay3", relay3, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(relays_state_t, "relay4", relay4, JSON_TOK_TRUE),
};

static int relays_state_handler(struct http_client_ctx *client,
                            enum http_transaction_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data) {

    ARG_UNUSED(user_data);

    static char resp_buf[256] = "\0";
    static char post_request_buff [256] = "\0";
    static size_t cursor;


    if (client->method == HTTP_GET) {
// State read from globar var
        LOG_INF("GET /api/relays/state");
        bool r[4] = {false, false, false, false};
        for (size_t i = 0; i < accessories_count; i++) {
            uint8_t ep = accessories_list[i].endpoint;
            if (ep >= 1 && ep <= 4) {
                r[ep - 1] = accessories_list[i].state;
            }
        }
        int n = snprintk(resp_buf, sizeof(resp_buf),
                         "{\"relay1\": %s, "
                         "\"relay2\": %s, "
                         "\"relay3\": %s, "
                         "\"relay4\": %s }",
                         r[0] ? "true" : "false",
                         r[1] ? "true" : "false",
                         r[2] ? "true" : "false",
                         r[3] ? "true" : "false");

        if (n < 0 || n >= (int)sizeof(resp_buf)) {
            response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
            static const char err_json[] = "{\"error\":\"format\"}";
            response_ctx->body = (uint8_t *)err_json;
            response_ctx->body_len = sizeof(err_json) - 1;
            response_ctx->final_chunk = true;

            return 0;
        }
        LOG_DBG("GET /api/relays/state: %s", resp_buf);
        response_ctx->status = HTTP_200_OK;
        response_ctx->body = (uint8_t *) resp_buf;
        response_ctx->body_len = strlen(resp_buf);
        response_ctx->final_chunk = true;

        return 0;

    }else if (client->method == HTTP_POST) {
        LOG_INF("POST /api/relays/state");

        if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
            LOG_WRN("POST /api/relays/state aborted");
            cursor = 0;
            return 0;
        }

        if (request_ctx->data_len + cursor > sizeof(post_request_buff)) {
            LOG_WRN("POST /api/relays/state: buffer overflow");
            cursor = 0;
            return -ENOMEM;
        }

        memcpy(post_request_buff + cursor, request_ctx->data, request_ctx->data_len);
        cursor += request_ctx->data_len;

        if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {

            LOG_DBG("POST /api/relays/state: %s", post_request_buff);

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


static struct http_resource_detail_dynamic relays_state_api = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST) | BIT(HTTP_OPTIONS),
        .content_type = "application/json"
    },
    .cb = relays_state_handler,
    .user_data = NULL,
};


HTTP_RESOURCE_DEFINE(api_relays_state,
                     http_service,
                     "/api/relays/state",
                     &relays_state_api);
