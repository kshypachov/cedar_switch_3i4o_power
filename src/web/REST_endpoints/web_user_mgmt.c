// //
// // Created by Kirill Shypachov on 19.10.2025.
// //
//
// #include <zephyr/kernel.h>
// #include <zephyr/net/http/service.h>
// #include <zephyr/net/http/server.h>
// #include <zephyr/settings/settings.h>
// #include <zephyr/sys/printk.h>
// #include <zephyr/data/json.h>
// #include <string.h>
// #include <zephyr/logging/log.h>
// #include <zephyr/data/json.h>
// #include "../http_common.h"
// #include "../../settings_topics.h"
// #include "../authentication.h"
//
// LOG_MODULE_REGISTER(REST_API_web_user_mgmt);
//
// #define RESPONSE_INTERNAL_SERVER_ERROR "Internal server error"
// #define RESPONSE_USER_UPDATED "User updated"
// #define RESPONSE_USER_NOT_FOUND "User not found"
// #define RESPONSE_USER_ALREADY_EXISTS "User already exists"
// #define RESPONSE_USER_DELETED "User deleted"
//
// #define JSON_BUF_SIZE (MAX_USERNAME_LEN + MAX_PASS_HASH_LEN) * MAX_USERS * 2
//
// typedef struct  {
//     char username[MAX_USERNAME_LEN];
//     char password[MAX_PASS_TEXT_LEN];
// } web_user_credentials_t;
//
// static const struct json_obj_descr web_user_cred_descr[] = {
//     JSON_OBJ_DESCR_PRIM_NAMED(web_user_credentials_t, "name", username, JSON_TOK_STRING_BUF),
//     JSON_OBJ_DESCR_PRIM_NAMED(web_user_credentials_t, "password", password, JSON_TOK_STRING_BUF)
// };
//
// static int web_user_mgmt_handler(struct http_client_ctx *client,
//                             enum http_transaction_status status,
//                             const struct http_request_ctx *request_ctx,
//                             struct http_response_ctx *response_ctx,
//                             void *user_data) {
//
// switch (client->method) {
//
//     case HTTP_GET: {
//         web_users_list_t web_users_list = {0};
//         if (0 == get_web_users(&web_users_list)) {
//             static char json_buff[JSON_BUF_SIZE];
//             size_t offset = 0;
//             int ret;
//
//             ret = snprintf(json_buff + offset, sizeof(json_buff) - offset, "{ \"users\": [");
//             offset += ret;
//
//             for (int i = 0; i < MAX_USERS; i++) {
//                 if (web_users_list.user[i].user[0] == '\0')
//                     continue; // пропускаем пустые
//
//                 ret = snprintf(json_buff  + offset, sizeof(json_buff) - offset,
//                                "%s{\"name\": \"%s\"}",
//                                (offset > 15 ? "," : ""), // добавляем запятую, кроме первого
//                                web_users_list.user[i].user);
//                 offset += ret;
//
//                 if (offset >= sizeof(json_buff) - 1)
//                     break; // защита от переполнения
//             }
//
//             snprintf(json_buff + offset, sizeof(json_buff) - offset, "] }");
//
//             response_ctx->status = HTTP_200_OK;
//             response_ctx->body = (uint8_t *)json_buff;
//             response_ctx->body_len = strlen(json_buff);
//             response_ctx->final_chunk = true;
//         }else {
//             response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
//             response_ctx->body = (uint8_t *)RESPONSE_INTERNAL_SERVER_ERROR;
//             response_ctx->body_len = strlen(RESPONSE_INTERNAL_SERVER_ERROR);
//             response_ctx->final_chunk = true;
//         }
//         break;
//     }
//
//     case HTTP_POST: {
//         static size_t cursor;
//         static char post_request_buff[JSON_BUF_SIZE];
//
//         if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
//             cursor = 0;
//             return 0;
//         }
//
//         if (request_ctx->data_len + cursor > sizeof(post_request_buff)) {
//             response_ctx->status = HTTP_413_PAYLOAD_TOO_LARGE;
//             static const char msg[] = "{\"error\":\"payload too large\"}";
//             response_ctx->body = (uint8_t *)msg;
//             response_ctx->body_len = sizeof(msg) - 1;
//             response_ctx->final_chunk = true;
//             cursor = 0;
//             return -ENOMEM;
//         }
//
//         memcpy(post_request_buff + cursor, request_ctx->data, request_ctx->data_len);
//         cursor += request_ctx->data_len;
//
//         if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
//             web_user_credentials_t tmp = {0};
//             const int expected = BIT_MASK(ARRAY_SIZE(web_user_cred_descr));
//             int ret = json_obj_parse(post_request_buff, cursor, web_user_cred_descr, ARRAY_SIZE(web_user_cred_descr), &tmp);
//
//             cursor = 0;
//
//             if (ret != expected) {
//                 /* Можно различать: <0 — ошибка парсера, иначе — битовая маска отсутствующих полей */
//                 response_ctx->status = HTTP_400_BAD_REQUEST;
//                 static const char msg[] = "{\"error\":\"invalid json or missing fields\"}";
//                 response_ctx->body = (uint8_t *)msg;
//                 response_ctx->body_len = sizeof(msg) - 1;
//                 response_ctx->final_chunk = true;
//                 return 0;
//             }
//
//             int result = add_web_user(tmp.username, tmp.password);
//
//             switch (result) {
//                 case 0: {
// //TODO the same part of code like GET answer, need to rewrite
//                     web_users_list_t web_users_list = {0};
//                     if (0 == get_web_users(&web_users_list)) {
//                         static char json_buff[JSON_BUF_SIZE];
//                         size_t offset = 0;
//                         int ret;
//
//                         ret = snprintf(json_buff + offset, sizeof(json_buff) - offset, "{ \"users\": [");
//                         offset += ret;
//
//                         for (int i = 0; i < MAX_USERS; i++) {
//                             if (web_users_list.user[i].user[0] == '\0')
//                                 continue; // пропускаем пустые
//
//                             ret = snprintf(json_buff  + offset, sizeof(json_buff) - offset,
//                                            "%s{\"name\": \"%s\"}",
//                                            (offset > 15 ? "," : ""), // добавляем запятую, кроме первого
//                                            web_users_list.user[i].user);
//                             offset += ret;
//
//                             if (offset >= sizeof(json_buff) - 1)
//                                 break; // защита от переполнения
//                         }
//
//                         snprintf(json_buff + offset, sizeof(json_buff) - offset, "] }");
//
//                         response_ctx->status = HTTP_200_OK;
//                         response_ctx->body = (uint8_t *)json_buff;
//                         response_ctx->body_len = strlen(json_buff);
//                         response_ctx->final_chunk = true;
//                     }else {
//                         response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
//                         static const char msg[] = "{\"error\":\"internal error\"}";
//                         response_ctx->body = (uint8_t *)msg;
//                         response_ctx->body_len = sizeof(msg) - 1;
//                     }
//
//                     break;
//                 }
//                 case -EEXIST: {
//                     response_ctx->status = HTTP_400_BAD_REQUEST;
//                     static const char msg[] = "{\"error\":\"user already exists\"}";
//                     response_ctx->body = (uint8_t *)msg;
//                     response_ctx->body_len = sizeof(msg) - 1;
//                     response_ctx->final_chunk = true;
//                     break;
//                 }
//                 case -ENOSPC: {
//                     response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
//                     static const char msg[] = "{\"error\":\"no space left, max number of users.\"}";
//                     response_ctx->body = (uint8_t *)msg;
//                     response_ctx->body_len = sizeof(msg) - 1;
//                     response_ctx->final_chunk = true;
//                     break;
//                 }
//                 default: {
//                     response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
//                     static const char msg[] = "{\"error\":\"unknown error\"}";
//                     response_ctx->body = (uint8_t *)msg;
//                     response_ctx->body_len = sizeof(msg) - 1;
//                     response_ctx->final_chunk = true;
//                     break;
//                 }
//             }
//         }
//
//         break;
//     }
//
//
//     case HTTP_DELETE: {
//         const char *url = client->url_buffer;  // например: "/api/auth/users?username=admin"
//         const char *param = strstr(url, "?");
//
//         if (!param) {
//             response_ctx->status = HTTP_400_BAD_REQUEST;
//             static const char msg[] = "{\"error\":\"missing parameters\"}";
//             response_ctx->body = (uint8_t *)msg;
//             response_ctx->body_len = sizeof(msg) - 1;
//             response_ctx->final_chunk = true;
//             break;
//         }
//
//         param++; // пропускаем '?'
//         const char *key = "name=";
//         const char *value = strstr(param, key);
//         if (!value) {
//             response_ctx->status = HTTP_400_BAD_REQUEST;
//             static const char msg[] = "{\"error\":\"parameter name required\"}";
//             response_ctx->body = (uint8_t *)msg;
//             response_ctx->body_len = sizeof(msg) - 1;
//             response_ctx->final_chunk = true;
//             break;
//         }
//         value += strlen(key);
//         char username[MAX_USERNAME_LEN] = {0};
//
//         /* Копируем значение до следующего '&' или конца строки */
//         size_t i = 0;
//         while (value[i] && value[i] != '&' && i < sizeof(username) - 1) {
//             username[i] = value[i];
//             i++;
//         }
//         username[i] = '\0';
//
//         LOG_INF("DELETE user: %s", username);
//
//         int res = delete_web_user(username);
//         switch (res) {
//             case 0: {
//                 //TODO the same part of code like GET answer, need to rewrite
//                 web_users_list_t web_users_list = {0};
//                 if (0 == get_web_users(&web_users_list)) {
//                     static char json_buff[JSON_BUF_SIZE];
//                     size_t offset = 0;
//                     int ret;
//
//                     ret = snprintf(json_buff + offset, sizeof(json_buff) - offset, "{ \"users\": [");
//                     offset += ret;
//
//                     for (int i = 0; i < MAX_USERS; i++) {
//                         if (web_users_list.user[i].user[0] == '\0')
//                             continue; // пропускаем пустые
//
//                         ret = snprintf(json_buff  + offset, sizeof(json_buff) - offset,
//                                        "%s{\"name\": \"%s\"}",
//                                        (offset > 15 ? "," : ""), // добавляем запятую, кроме первого
//                                        web_users_list.user[i].user);
//                         offset += ret;
//
//                         if (offset >= sizeof(json_buff) - 1)
//                             break; // защита от переполнения
//                     }
//
//                     snprintf(json_buff + offset, sizeof(json_buff) - offset, "] }");
//
//                     response_ctx->status = HTTP_200_OK;
//                     response_ctx->body = (uint8_t *)json_buff;
//                     response_ctx->body_len = strlen(json_buff);
//                     response_ctx->final_chunk = true;
//                 }else {
//                     response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
//                     static const char msg[] = "{\"error\":\"internal error\"}";
//                     response_ctx->body = (uint8_t *)msg;
//                     response_ctx->body_len = sizeof(msg) - 1;
//                 }
//
//                 break;
//             }
//
//             case -ENOENT: {
//                 response_ctx->status = HTTP_404_NOT_FOUND;
//                 static const char msg[] = "{\"error\":\"user not found\"}";
//                 response_ctx->body = (uint8_t *)msg;
//                 response_ctx->body_len = sizeof(msg) - 1;
//                 response_ctx->final_chunk = true;
//                 break;
//             }
//
//             case -EPERM: {
//                 response_ctx->status = HTTP_403_FORBIDDEN;
//                 static const char msg[] = "{\"error\":\"can't delete last user\"}";
//                 response_ctx->body = (uint8_t *)msg;
//                 response_ctx->body_len = sizeof(msg) - 1;
//                 response_ctx->final_chunk = true;
//                 break;
//             }
//
//             default: {
//                 response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
//                 static const char msg[] = "{\"error\":\"internal error\"}";
//                 response_ctx->body = (uint8_t *)msg;
//                 response_ctx->body_len = sizeof(msg) - 1;
//                 response_ctx->final_chunk = true;
//                 break;
//             }
//         }
//
//         break;
//     }
//
//
//     default:
//         return -1;
// }
//
//     return 0;
// }
//
//
// static struct http_resource_detail_dynamic web_user = {
//     .common = {
//         .type = HTTP_RESOURCE_TYPE_DYNAMIC,
//         .bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST) | BIT(HTTP_DELETE),
//         .content_type = "application/json"
//     },
//     .cb = web_user_mgmt_handler,
//     .user_data = NULL,
// };
//
// /* === Register path for HTTP service only === */
// HTTP_RESOURCE_DEFINE(api_web_user_mgmt,
//                      http_api_service,
//                      "/api/auth/users",
//                      &web_user);
