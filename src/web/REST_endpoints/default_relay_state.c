//
// Created by Kirill Shypachov on 15.09.2025.
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
#include "../authentication.h"

LOG_MODULE_REGISTER(REST_API_default_relay_state);


static void app_settings_def_relay_state_update(relays_def_state state) {

    settings_save_one(def_relays_state_enable_settings_topik, &state.enabled, sizeof(state.enabled));
    settings_save_one(def_relays1_state_settings_topik, &state.relay1, sizeof(state.relay1));
    settings_save_one(def_relays2_state_settings_topik, &state.relay2, sizeof(state.relay2));
    settings_save_one(def_relays3_state_settings_topik, &state.relay3, sizeof(state.relay3));
}

/* Дескрипторы полей для json_obj_parse:
 * Используем *_NAMED, чтобы сопоставить имена ключей JSON с именами полей структуры.
 */
static const struct json_obj_descr relays_def_state_descr[] = {
    JSON_OBJ_DESCR_PRIM_NAMED(relays_def_state, "enabled", enabled, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(relays_def_state, "relay1", relay1, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(relays_def_state, "relay2", relay2, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(relays_def_state, "relay3", relay3, JSON_TOK_TRUE),
};

static int settings_def_relay_state_handler(struct http_client_ctx *client,
                            enum http_data_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data) {

    //ARG_UNUSED(request_ctx);
    ARG_UNUSED(user_data);

    static char resp_buf[256] = "\0";
    static char post_request_buff [256] = "\0";
    static size_t cursor;
    //authentication(headers, header_count, )
    //if (status == HTTP_SERVER_DATA_FINAL) {

        if (client->method == HTTP_GET) {

            if (status == HTTP_SERVER_DATA_FINAL) {

                relays_def_state relays_def = {0};

                settings_load_one(def_relays_state_enable_settings_topik, &relays_def.enabled, sizeof(relays_def.enabled));
                settings_load_one(def_relays1_state_settings_topik, &relays_def.relay1, sizeof(relays_def.relay1));
                settings_load_one(def_relays2_state_settings_topik, &relays_def.relay2, sizeof(relays_def.relay2));
                settings_load_one(def_relays3_state_settings_topik, &relays_def.relay3, sizeof(relays_def.relay3));

                /* Формируем JSON строку */
                snprintk(resp_buf, sizeof(resp_buf),
                                 "{\"enabled\": %s, "
                                 "\"relay1\": %s, "
                                 "\"relay2\": %s, "
                                 "\"relay3\": %s}",
                                 relays_def.enabled ? "true" : "false",
                                 relays_def.relay1 ? "true" : "false",
                                 relays_def.relay2 ? "true" : "false",
                                 relays_def.relay3 ? "true" : "false");
            }
        }else if (client->method == HTTP_POST) {
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
                http_settings_status_set_updated();
                relays_def_state tmp = {0};
                const int expected = BIT_MASK(ARRAY_SIZE(relays_def_state_descr));
                int ret = json_obj_parse(post_request_buff, cursor, relays_def_state_descr, ARRAY_SIZE(relays_def_state_descr), &tmp);

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
                /* Применяем новое состояние */
                app_settings_def_relay_state_update(tmp);

                /* Возвращаем обновлённое состояние (или 204 No Content — на ваш вкус) */
                int n = snprintk(resp_buf, sizeof(resp_buf),
                                 "{\"enabled\": %s, "
                                 "\"relay1\": %s, "
                                 "\"relay2\": %s, "
                                 "\"relay3\": %s}",
                                 tmp.enabled ? "true" : "false",
                                 tmp.relay1 ? "true" : "false",
                                 tmp.relay2 ? "true" : "false",
                                 tmp.relay3 ? "true" : "false");

                if (n < 0 || n >= (int)sizeof(resp_buf)) {
                    response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
                    static const char err_json[] = "{\"error\":\"format\"}";
                    response_ctx->body = (uint8_t *)err_json;
                    response_ctx->body_len = sizeof(err_json) - 1;
                    response_ctx->final_chunk = true;

                    return 0;
                }

                response_ctx->status = HTTP_200_OK; /* альтернативно: HTTP_STATUS_NO_CONTENT */
                response_ctx->body = (uint8_t *)resp_buf;
                response_ctx->body_len = (size_t)n;
                response_ctx->final_chunk = true;

                return 0;

            }

            return 0;

        }else {
            return -1;
        }

    response_ctx->body = (uint8_t *)resp_buf;
    response_ctx->body_len = strlen(resp_buf);
    response_ctx->final_chunk = true;
    //}
    return 0;
}


static struct http_resource_detail_dynamic settings_def_relay_state = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
        .content_type = "application/json"
    },
    .cb = settings_def_relay_state_handler,
    .user_data = NULL,
};

/* === Register path for HTTP service only === */
HTTP_RESOURCE_DEFINE(api_def_relay_state,
                     http_api_service,
                     "/api/relays/safe_state",
                     &settings_def_relay_state);