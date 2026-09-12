// //
// // Created by Kirill Shypachov on 18.10.2025.
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
// #include "../http_common.h"
// #include "../../settings_topics.h"
// #include "../authentication.h"
//
// LOG_MODULE_REGISTER(REST_API_login);
//
// #define RESPONSE_TOKEN_VALID_MESSAGE "Session token or username password valid"
// #define RESPONSE_TOKEN_INVALID_MESSAGE "Session token or username password invalid"
//
// static int login_handler(struct http_client_ctx *client,
//                             enum http_transaction_status status,
//                             const struct http_request_ctx *request_ctx,
//                             struct http_response_ctx *response_ctx,
//                             void *user_data) {
//
//     if (client->method == HTTP_GET) {
//
//         size_t header_count;
//
//         header_count = request_ctx->header_count;
//         const struct http_header *headers = request_ctx->headers;
//
//         LOG_INF("Header count: %d",  header_count);
//
//         for (uint32_t i = 0; i < header_count; i++) {
//             LOG_INF("Header: '%s: %s'", headers[i].name, headers[i].value);
//         }
//
//         if (0 <= authentication(request_ctx->headers, request_ctx->header_count, NULL, 0)) {
//             response_ctx->status = HTTP_200_OK;
//             response_ctx->body = (const uint8_t *)RESPONSE_TOKEN_VALID_MESSAGE;
//             response_ctx->body_len = strlen(RESPONSE_TOKEN_VALID_MESSAGE);
//             response_ctx->final_chunk = true;
//         }else {
//             response_ctx->status = HTTP_401_UNAUTHORIZED;
//             response_ctx->body = (const uint8_t *)RESPONSE_TOKEN_INVALID_MESSAGE;
//             response_ctx->body_len = strlen(RESPONSE_TOKEN_INVALID_MESSAGE);
//             response_ctx->final_chunk = true;
//         }
//
//     }else if (client->method == HTTP_POST) {
//
//         char session_token[SESSION_TOKEN_SIZE];
//
//         if (0 <= authentication(request_ctx->headers, request_ctx->header_count, session_token, SESSION_TOKEN_SIZE)) {
//             LOG_INF("Session token: %s", session_token);
//
//             static char cookie_header[256];
//             snprintf(cookie_header, sizeof(cookie_header), "session_token=%s; Path=/; HttpOnly; Max-Age=86400", session_token);
//
//             static struct http_header response_headers[] = {
//                 {.name = "Set-Cookie", .value = cookie_header}
//             };
//
//             response_ctx->headers = response_headers;
//             response_ctx->header_count = 1;
//             response_ctx->status = HTTP_200_OK;
//             response_ctx->body = (const uint8_t *)RESPONSE_TOKEN_VALID_MESSAGE;
//             response_ctx->body_len = strlen(RESPONSE_TOKEN_VALID_MESSAGE);
//             response_ctx->final_chunk = true;
//         }else {
//             LOG_ERR("Authentication failed");
//             response_ctx->status = HTTP_401_UNAUTHORIZED;
//             response_ctx->body = (const uint8_t *)RESPONSE_TOKEN_INVALID_MESSAGE;
//             response_ctx->body_len = strlen(RESPONSE_TOKEN_INVALID_MESSAGE);
//             response_ctx->final_chunk = true;
//
//         }
//     }
//
//     return 0;
// }
//
//
// static struct http_resource_detail_dynamic login = {
//     .common = {
//         .type = HTTP_RESOURCE_TYPE_DYNAMIC,
//         .bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
//         .content_type = "application/json"
//     },
//     .cb = login_handler,
//     .user_data = NULL,
// };
//
// /* === Register path for HTTP service only === */
// HTTP_RESOURCE_DEFINE(api_login,
//                      http_api_service,
//                      "/api/auth/login",
//                      &login);