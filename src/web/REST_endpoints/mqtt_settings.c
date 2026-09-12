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
#include <settings_registry/settings_registry.h>

LOG_MODULE_REGISTER(REST_API_mqtt_settings);

/*
 * MQTT connection settings, persisted through settings-registry under
 * "reg/mqtt/". Keys that were written by earlier firmware as raw Zephyr
 * settings ("/settings/mqtt/...") are not migrated: nothing on the device
 * reads MQTT settings at runtime (the loader in ha_mqtt.c is commented out),
 * so an existing board shows defaults until the settings are posted again.
 * "secure" was declared but never stored, so it has no key.
 */
#define MQTT_KEY_ENABLED "mqtt/enabled"
#define MQTT_KEY_HOST    "mqtt/host"
#define MQTT_KEY_PORT    "mqtt/port"
#define MQTT_KEY_USER    "mqtt/user"
#define MQTT_KEY_PASS    "mqtt/pass"

static bool mqtt_enabled_storage;
static uint32_t mqtt_port_storage;
static char mqtt_host_storage[sizeof(((mqtt_settings_t *)0)->host)];
static char mqtt_user_storage[sizeof(((mqtt_settings_t *)0)->user)];
static char mqtt_pass_storage[sizeof(((mqtt_settings_t *)0)->pass)];

SETTING_REGISTRY_DEFINE(mqtt_enabled_setting, .key = MQTT_KEY_ENABLED,
                        .type = SETTING_TYPE_BOOL, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .ram_storage = &mqtt_enabled_storage);
SETTING_REGISTRY_DEFINE(mqtt_host_setting, .key = MQTT_KEY_HOST,
                        .type = SETTING_TYPE_STRING, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .max_len = sizeof(mqtt_host_storage), .default_value = "",
                        .ram_storage = mqtt_host_storage);
/* The JSON field is uint16_t: reject what would silently wrap. */
SETTING_REGISTRY_DEFINE(mqtt_port_setting, .key = MQTT_KEY_PORT,
                        .type = SETTING_TYPE_U32, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .ram_storage = &mqtt_port_storage,
                        .validate = setting_validate_range_u32,
                        .validate_ctx = &(struct setting_range_u32){ .min = 0, .max = UINT16_MAX });
SETTING_REGISTRY_DEFINE(mqtt_user_setting, .key = MQTT_KEY_USER,
                        .type = SETTING_TYPE_STRING, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .max_len = sizeof(mqtt_user_storage), .default_value = "",
                        .ram_storage = mqtt_user_storage);
SETTING_REGISTRY_DEFINE(mqtt_pass_setting, .key = MQTT_KEY_PASS,
                        .type = SETTING_TYPE_STRING, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .max_len = sizeof(mqtt_pass_storage), .default_value = "",
                        .ram_storage = mqtt_pass_storage);

static int mqtt_set_string(const char *key, const char *value)
{
    struct setting_value v = {
        .type = SETTING_TYPE_STRING,
        .buf = { .data = (uint8_t *)value, .len = strlen(value) },
    };

    return setting_set(key, &v);
}

static void mqtt_get_string(const char *key, char *buf, size_t size)
{
    struct setting_value v = {
        .type = SETTING_TYPE_STRING,
        .buf = { .data = (uint8_t *)buf, .len = size },
    };

    if (setting_get(key, &v) != 0) {
        buf[0] = '\0';
        return;
    }
    buf[MIN(v.buf.len, size - 1)] = '\0';
}

/* Returns 0, or the first registry error; later keys are still attempted. */
static int app_settings_mqtt_update(mqtt_settings_t const *mqtt) {
    int first_err = 0;
    int rc;

    rc = setting_set(MQTT_KEY_ENABLED, &(struct setting_value){
        .type = SETTING_TYPE_BOOL, .b = mqtt->enabled });
    first_err = first_err ? first_err : rc;
    rc = mqtt_set_string(MQTT_KEY_HOST, mqtt->host);
    first_err = first_err ? first_err : rc;
    rc = setting_set(MQTT_KEY_PORT, &(struct setting_value){
        .type = SETTING_TYPE_U32, .u32 = mqtt->port });
    first_err = first_err ? first_err : rc;
    rc = mqtt_set_string(MQTT_KEY_USER, mqtt->user);
    first_err = first_err ? first_err : rc;
    rc = mqtt_set_string(MQTT_KEY_PASS, mqtt->pass);
    first_err = first_err ? first_err : rc;

    if (first_err != 0) {
        LOG_ERR("failed to store MQTT settings: %d", first_err);
    }
    return first_err;
}

/* Load all fields; a key that cannot be read keeps its zero value */
static void app_settings_mqtt_load(mqtt_settings_t *mqtt) {
    struct setting_value v;

    memset(mqtt, 0, sizeof(*mqtt));

    v.type = SETTING_TYPE_BOOL;
    if (setting_get(MQTT_KEY_ENABLED, &v) == 0) {
        mqtt->enabled = v.b;
    }
    v.type = SETTING_TYPE_U32;
    if (setting_get(MQTT_KEY_PORT, &v) == 0) {
        mqtt->port = (uint16_t)v.u32;
    }
    mqtt_get_string(MQTT_KEY_HOST, mqtt->host, sizeof(mqtt->host));
    mqtt_get_string(MQTT_KEY_USER, mqtt->user, sizeof(mqtt->user));
    mqtt_get_string(MQTT_KEY_PASS, mqtt->pass, sizeof(mqtt->pass));
}

static const struct json_obj_descr mqtt_settings_descr[] = {
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "enabled", enabled, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "host", host, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "port", port, JSON_TOK_INT),
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "user", user, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM_NAMED(mqtt_settings_t, "pass", pass, JSON_TOK_STRING_BUF),
};

static int settings_mqtt_handler(struct http_client_ctx *client,
                            enum http_transaction_status status,
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
        if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {

            app_settings_mqtt_load(&mqtt_sett);

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

        if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
            cursor = 0;
            return 0;
        }

        if (request_ctx->data_len + cursor > sizeof(post_request_buff)) {
            cursor = 0;
            return -ENOMEM;
        }

        memcpy(post_request_buff + cursor, request_ctx->data, request_ctx->data_len);
        cursor += request_ctx->data_len;

        if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {

            //http_settings_status_set_updated();
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
            if (app_settings_mqtt_update(&tmp) != 0) {
                response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
                static const char err_json[] = "{\"error\":\"storage\"}";
                response_ctx->body = (uint8_t *)err_json;
                response_ctx->body_len = sizeof(err_json) - 1;
                response_ctx->final_chunk = true;

                return 0;
            }

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
                     http_service,
                     "/api/mqtt/settings",
                     &settings_mqtt);