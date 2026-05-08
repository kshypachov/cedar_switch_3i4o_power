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

LOG_MODULE_REGISTER(REST_API_mqtt_settings);

static void app_settings_mqtt_update(mqtt_settings_t const mqtt) {
    settings_save_one(mqtt_enabled_settings, &mqtt.enabled, sizeof(mqtt.enabled));
    settings_save_one(mqtt_host_settings, mqtt.host, sizeof(mqtt.host));
    settings_save_one(mqtt_port_settings, &mqtt.port, sizeof(mqtt.port));
    settings_save_one(mqtt_user_settings, mqtt.user, sizeof(mqtt.user));
    settings_save_one(mqtt_pass_settings, mqtt.pass, sizeof(mqtt.pass));
}

/* Load all fields (defaults to zeroed if not present) */
static void app_settings_mqtt_load(mqtt_settings_t *mqtt) {
    memset(mqtt, 0, sizeof(*mqtt));
    settings_load_one(mqtt_enabled_settings, &mqtt->enabled, sizeof(mqtt->enabled));
    settings_load_one(mqtt_host_settings,   mqtt->host, sizeof(mqtt->host));
    settings_load_one(mqtt_port_settings,   &mqtt->port, sizeof(mqtt->port));
    settings_load_one(mqtt_user_settings,   mqtt->user, sizeof(mqtt->user));
    settings_load_one(mqtt_pass_settings,   mqtt->pass, sizeof(mqtt->pass));
}

static const struct json_obj_descr mqtt_settings_descr[] = {
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "enabled", enabled, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "host", host, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "port", port, JSON_TOK_INT),
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "user", user, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "pass", pass, JSON_TOK_STRING_BUF),
};

static int settings_mqtt_handler(struct http_client_ctx *client,
                            enum http_data_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data) {
    //ARG_UNUSED(request_ctx);
    ARG_UNUSED(user_data);

    static char resp_buf[256] = "\0";
    static char post_request_buff [256] = "\0";
    static size_t cursor;
    mqtt_settings_t mqtt_sett = {0};

    if (client->method == HTTP_GET) {
        if (status == HTTP_SERVER_DATA_FINAL) {

            app_settings_mqtt_load(&mqtt_sett);

            settings_load_one(mqtt_enabled_settings, &mqtt_sett.enabled, sizeof(mqtt_sett.enabled));
            settings_load_one(mqtt_host_settings, mqtt_sett.host, sizeof(mqtt_sett.host));
            settings_load_one(mqtt_port_settings, &mqtt_sett.port, sizeof(mqtt_sett.port));
            settings_load_one(mqtt_user_settings, mqtt_sett.user, sizeof(mqtt_sett.user));
            settings_load_one(mqtt_pass_settings, mqtt_sett.pass, sizeof(mqtt_sett.pass));


            /* Формируем JSON строку */
            int n = snprintk(resp_buf, sizeof(resp_buf),
                             "{\"enabled\": %s, "
                             "\"host\": \"%s\", "
                             "\"port\": %d, "
                             "\"user\": \"%s\", "
                             "\"pass\": \"%s\"}",
                             mqtt_sett.enabled ? "true" : "false",
                             mqtt_sett.host,
                             mqtt_sett.port,
                             mqtt_sett.user,
                             strlen(mqtt_sett.user) ? "*****" : "");

            if (n < 0 || n >= (int)sizeof(resp_buf)) {
                response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
                static const char err_json[] = "{\"error\":\"format\"}";
                response_ctx->body = (uint8_t *)err_json;
                response_ctx->body_len = sizeof(err_json) - 1;
                response_ctx->final_chunk = true;

                return 0;
            }

            response_ctx->status = HTTP_200_OK;
            response_ctx->body = (uint8_t *)resp_buf;
            response_ctx->body_len = strlen(resp_buf);
            response_ctx->final_chunk = true;

            return 0;

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
            mqtt_settings_t tmp = {0};
            const int expected = BIT_MASK(ARRAY_SIZE(mqtt_settings_descr));
            int ret = json_obj_parse(post_request_buff, cursor, mqtt_settings_descr, ARRAY_SIZE(mqtt_settings_descr), &tmp);

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
            app_settings_mqtt_update(tmp);

            /* Возвращаем обновлённое состояние (или 204 No Content — на ваш вкус) */
            int n = snprintk(resp_buf, sizeof(resp_buf),
                             "{\"enabled\": %s, "
                             "\"host\": \"%s\", "
                             "\"port\": %d, "
                             "\"user\": \"%s\", "
                             "\"pass\": \"%s\"}",
                             tmp.enabled ? "true" : "false",
                             tmp.host ,
                             tmp.port,
                             tmp.user,
                             strlen(tmp.pass) ? "****" : "");

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

    return 0;
}


static struct http_resource_detail_dynamic settings_mqtt = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
        .content_type = "application/json"
    },
    .cb = settings_mqtt_handler,
    .user_data = NULL,
};

/* === Register path for HTTP service only === */
HTTP_RESOURCE_DEFINE(api_mqtt_settings,
                     http_api_service,
                     "/api/mqtt/settings",
                     &settings_mqtt);