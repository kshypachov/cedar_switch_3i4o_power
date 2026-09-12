// //
// // Created by Kirill Shypachov on 15.09.2025.
// //
//
// #ifndef CEDAR_SWITCH_3IN3OUT_POWER_HTTP_COMMON_H
// #define CEDAR_SWITCH_3IN3OUT_POWER_HTTP_COMMON_H
//
// #include <zephyr/net/http/server.h>
// #include <zephyr/net/http/service.h>
//
// int http_settings_status_get(void);
// void http_settings_status_set_updated(void);
//
// int http_upload(struct http_client_ctx *client,
//     enum http_transaction_status status,
//     const struct http_request_ctx *request_ctx,
//     struct http_response_ctx *response_ctx,
//     char *base_path,
//     size_t request_body_len,
//     char *post_request_buff,
//     size_t post_request_buff_len);
//
// #endif //CEDAR_SWITCH_3IN3OUT_POWER_HTTP_COMMON_H