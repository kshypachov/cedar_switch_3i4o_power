// //
// // Created by Kirill Shypachov on 15.09.2025.
// //
//
// #include <zephyr/kernel.h>
// #include <zephyr/net/http/service.h>
// #include <zephyr/net/http/server.h>
// #include <string.h>
// #include <zephyr/logging/log.h>
// #include "../http_common.h"
//
// LOG_MODULE_REGISTER(REST_API_settings_status);
//
//
// /* Буфер под JSON */
// static char resp_buf[64] = "\0";
//
// /* === ADD: dynamic JSON endpoint GET /api/settings/ === */
//
// static int settings_handler_get(struct http_client_ctx *client,
//                             enum http_data_status status,
//                             const struct http_request_ctx *request_ctx,
//                             struct http_response_ctx *response_ctx,
//                             void *user_data)
// {
//     ARG_UNUSED(client);
//     ARG_UNUSED(request_ctx);
//     ARG_UNUSED(user_data);
//
//     if (status == HTTP_SERVER_DATA_FINAL) {
//         if (http_settings_status_get()) {
//             sprintf(resp_buf, "{\"reboot_required\": true}");
//         }else {
//             sprintf(resp_buf, "{\"reboot_required\": false}");
//         }
//         response_ctx->body = (uint8_t *)resp_buf;
//         response_ctx->body_len = strlen(resp_buf);
//         response_ctx->final_chunk = true;
//         /* Если доступно, можно указать content-type */
//         /* response_ctx->content_type = "application/json"; */
//     }
//     return 0;
// }
//
// static struct http_resource_detail_dynamic settings_resource_detail_get = {
//     .common = {
//         .type = HTTP_RESOURCE_TYPE_DYNAMIC,
//         .bitmask_of_supported_http_methods = BIT(HTTP_GET),
//         .content_type = "application/json"
//     },
//     .cb = settings_handler_get,
//     .user_data = NULL,
// };
//
// /* === Register path for HTTP service only === */
// HTTP_RESOURCE_DEFINE(api_settings_status_get,
//                      http_api_service,
//                      "/api/system/reboot_required",
//                      &settings_resource_detail_get);