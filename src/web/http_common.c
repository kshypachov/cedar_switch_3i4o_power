// //
// // Created by Kirill Shypachov on 15.09.2025.
// //
//
// #include "http_common.h"
//
// #include <stdbool.h>
// #include <string.h>
// #include <stdbool.h>
// #include <zephyr/fs/fs.h>
// #include <zephyr/net/http/server.h>
// #include <zephyr/net/http/service.h>
// #include <zephyr/logging/log.h>
// #include <string.h>
// #include <errno.h>
//
//
// //settings functions
// static int updated = 0;
//
// void http_settings_status_set_updated(void) {
//     updated = 1;
// }
//
// int http_settings_status_get(void) {
//     return updated;
// }
//
// //file upload functions
// static bool path_is_sane(const char *rel) {
//     if (!rel || !*rel) return false;
//     if (rel[0] == '/') return false;
//     for (const char *p = rel; *p; ) {
//         const char *slash = strchr(p, '/');
//         size_t seglen = slash ? (size_t)(slash - p) : strlen(p);
//         if (seglen == 0) return false;
//         if (seglen == 1 && p[0] == '.') return false;
//         if (seglen == 2 && p[0] == '.' && p[1] == '.') return false;
//         if (!slash) break;
//         p = slash + 1;
//     }
//     return true;
// }
//
// static bool query_get(const char *url, const char *key, char *out, size_t out_sz) {
//     const char *q = strchr(url, '?');
//     if (!q) return false;
//     q++;
//     size_t klen = strlen(key);
//     while (*q) {
//         const char *eq = strchr(q, '=');
//         const char *amp = strchr(q, '&');
//         if (!eq) break;
//         if (!amp) amp = q + strlen(q);
//         if ((size_t)(eq - q) == klen && strncmp(q, key, klen) == 0) {
//             size_t vlen = (size_t)(amp - (eq + 1));
//             if (vlen >= out_sz) vlen = out_sz - 1;
//             memcpy(out, eq + 1, vlen);
//             out[vlen] = 0;
//             return true;
//         }
//         q = (*amp) ? amp + 1 : amp;
//     }
//     return false;
// }
//
// #define CHUNK_BUF_CAP 512
// #define MAX_SIZE   (150 * 1024)
//
// int http_upload(struct http_client_ctx *client,
//     enum http_data_status status,
//     const struct http_request_ctx *request_ctx,
//     struct http_response_ctx *response_ctx,
//     char *base_path,
//     size_t request_body_len,
//     char *post_request_buff,
//     size_t post_request_buff_len) {
//
//
//     char file[192] = {0};
//     char offsetStr[32] = {0};
//     long offset = 0;
//     static char abs_path[256];
//     static char post_response_buff [CHUNK_BUF_CAP] = "\0";
//     int rc;
//
//
//     if (!query_get(client->url_buffer, "file", file, sizeof(file))) {
//         response_ctx->status = HTTP_400_BAD_REQUEST;
//         static const char err[] = "{\"error\":\"file required\"}";
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     if (!path_is_sane(file)) {
//         response_ctx->status = HTTP_400_BAD_REQUEST;
//         static const char err[] = "{\"error\":\"invalid file\"}";
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     if (!query_get(client->url_buffer, "offset", offsetStr, sizeof(offsetStr))) {
//         response_ctx->status = HTTP_400_BAD_REQUEST;
//         static const char err[] = "{\"error\":\"offset required\"}";
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     offset = strtol(offsetStr, NULL, 0);
//     if (offset < 0) {
//         response_ctx->status = HTTP_400_BAD_REQUEST;
//         static const char err[] = "{\"error\":\"offset < 0\"}";
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     if (offset + request_body_len > MAX_SIZE) {
//         response_ctx->status = HTTP_400_BAD_REQUEST;
//         static char err[256];
//         snprintf(err, sizeof(err), "{\"error\":\"%s over max size of %d\"}", file, MAX_SIZE);
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     int n = snprintf(abs_path, sizeof(abs_path), "%s/%s", base_path, file);
//     if (n < 0 || n >= (int)sizeof(abs_path)) {
//         response_ctx->status = HTTP_400_BAD_REQUEST;
//         static const char err[] = "{\"error\":\"path too long\"}";
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     if (offset == 0) {
//         (void)fs_unlink(abs_path);  // truncate semantics
//     }
//
//     struct fs_dirent fs_file_stat;
//     rc = fs_stat(abs_path, &fs_file_stat);
//     if ((fs_file_stat.type != FS_DIR_ENTRY_FILE) && (rc >=0)) {
//         response_ctx->status = HTTP_400_BAD_REQUEST;
//         static  char err[256];
//         snprintf(err, sizeof(err), "{\"error\":\"%s is not a file, it's dir\"}", file);
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     if (offset > 0 && offset != fs_file_stat.size) {
//         response_ctx->status = HTTP_400_BAD_REQUEST;
//         static  char err[256];
//         snprintf(err, sizeof(err), "{\"error\":\"%s: offset mismatch\"}", file);
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     struct fs_file_t fd;
//     fs_file_t_init(&fd);
//     rc = fs_open(&fd, abs_path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
//
//     if (rc < 0) {
//         response_ctx->status = HTTP_400_BAD_REQUEST;
//         static  char err[256];
//         snprintf(err, sizeof(err), "{\"error\":\"open(%s): %d\"}", file, rc);
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     ssize_t w_res = fs_write(&fd, post_request_buff, request_body_len);
//
//     if (w_res < 0 || (size_t)w_res != request_body_len) {
//         response_ctx->status = HTTP_500_INTERNAL_SERVER_ERROR;
//         static  char err[256];
//         snprintf(err, sizeof(err), "{\"error\":\"write(%s): %d\"}", file, w_res);
//         response_ctx->body = (const uint8_t *)err;
//         response_ctx->body_len = strlen(err);
//         response_ctx->final_chunk = true;
//         fs_close(&fd);
//
//         request_body_len = 0;
//         return 0;
//     }
//
//     long res = offset + w_res;
//     (void)fs_sync(&fd);
//     fs_close(&fd);
//
//     response_ctx->status = HTTP_200_OK;
//     snprintf(post_response_buff, sizeof(post_response_buff), "%lu", res);
//     response_ctx->body = (const uint8_t *)post_response_buff;
//     response_ctx->body_len = strlen(post_response_buff);
//     response_ctx->final_chunk = true;
//
//     return 0;
// }