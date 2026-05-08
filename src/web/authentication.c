// //
// // Created by Kirill Shypachov on 16.10.2025.
// //
//
//
//
// #include "zephyr/settings/settings.h"
// #include "zephyr/logging/log.h"
// #include "zephyr/sys/base64.h"
// #include <zephyr/sys/hash_function.h>
// #include <errno.h>
// #include <strings.h>
// #include <mbedtls/sha256.h>
// #include <zephyr/random/random.h>   // sys_csrand_get()
// #include <zephyr/sys/timeutil.h>
// #include <time.h>
//
//
//
//
// #include "authentication.h"
//
// LOG_MODULE_REGISTER(authentication);
//
// #define MAX_SESSIONS 10
// #define SESSION_LIFETIME_SEC 3600
//
// #define DEFAULT_USERNAME "admin"
// #define DEFAULT_PASSWORD "pass"
//
// typedef struct {
//     uint32_t expires;
//     char token[128];
// }web_session_t;
//
// static web_users_list_t web_users_list;
// static web_session_t web_session_list[MAX_SESSIONS];
//
//
//
// int get_web_users (web_users_list_t *users_list) {
//
//     if (users_list == NULL) {
//         return -EINVAL;
//     }
//
//     if (0 >= settings_load_one(web_users_settings, users_list, sizeof(*users_list))) {
//         return -EIO;
//     }
//
//     for (int i = 0; i < MAX_USERS; i++) {
//         memset(users_list->user[i].pass_hash, 0, sizeof(*users_list->user->pass_hash));
//     }
//
//     return 0;
// }
//
// static int update_users (const web_users_list_t *users_list) {
//
//     if (users_list == NULL) {
//         return -EINVAL;
//     }
//
//     if (users_list->user[0].user[0] == '\0') {
//         return -EINVAL;
//     }
//
//     if (0 != settings_save_one(web_users_settings, users_list, sizeof(*users_list))) {
//         return -EIO;
//     }
//
//     return 0;
// }
//
//
// /**
//  * @brief Вычислить SHA256-хеш от пароля и вернуть его в hex-виде.
//  */
// static int calc_password_hash(const char *password,  unsigned char *hash_hex, size_t hash_hex_size)
// {
//     if (hash_hex_size < 32) {
//         return -EINVAL;
//     }
//     memset(hash_hex, 0, hash_hex_size);
//     int ret = mbedtls_sha256((const unsigned char *)password, strlen(password), hash_hex, 0);
//
//     return ret;
// }
//
//
// /*
//  *Initialize authentication, check if settings has auth info,
//  * else init with default auth data
//  */
// void authentication_init(void) {
//
//     if (0 >= settings_load_one(web_users_settings, &web_users_list, sizeof(web_users_list))) {
//         web_users_list.changed = false;
//
//         LOG_INF("Authentication settings not found, init with default data");
//
//         strncpy(web_users_list.user[0].user, DEFAULT_USERNAME, sizeof(web_users_list.user[0].user));
//         unsigned char pass_hash_hex[32];
//         calc_password_hash(DEFAULT_PASSWORD, pass_hash_hex, sizeof(pass_hash_hex));
//         memcpy(web_users_list.user[0].pass_hash, pass_hash_hex, sizeof(pass_hash_hex));
//         settings_save_one(web_users_settings, &web_users_list, sizeof(web_users_list));
//     }
//     memset(web_session_list, 0, sizeof(web_session_list));
//     LOG_INF("Authentication initialized");
// }
//
// static void generate_token(const char *username, char *token_out, size_t token_size)
// {
//     uint8_t random_bytes[8];
//     sys_csrand_get(random_bytes, sizeof(random_bytes));
//
//     uint8_t hash_bin[32];
//     char input[280];
//
//     snprintf(input, sizeof(input), "%s_%u_%02x%02x%02x%02x",
//              username, (uint32_t)time(NULL),
//              random_bytes[0], random_bytes[1], random_bytes[2], random_bytes[3]);
//
//     mbedtls_sha256((const unsigned char *)input, strlen(input), hash_bin, 0);
//
//     for (int i = 0; i < 16 && (i * 2 + 1) < token_size; i++) {
//         snprintf(&token_out[i * 2], token_size - i * 2, "%02x", hash_bin[i]);
//     }
//     token_out[32] = '\0';
// }
//
// static void store_session_token(const char *token)
// {
//     uint32_t now = (uint32_t)time(NULL);
//
//     // ищем пустую ячейку
//     for (size_t i = 0; i < MAX_SESSIONS; i++) {
//         if (web_session_list[i].token[0] == '\0') {
//             strncpy(web_session_list[i].token, token, sizeof(web_session_list[i].token));
//             web_session_list[i].expires = now + SESSION_LIFETIME_SEC;
//             return;
//         }
//     }
//
//     // если нет пустых — заменяем самую старую
//     size_t oldest_idx = 0;
//     uint32_t oldest_time = web_session_list[0].expires;
//     for (size_t i = 1; i < MAX_SESSIONS; i++) {
//         if (web_session_list[i].expires < oldest_time) {
//             oldest_time = web_session_list[i].expires;
//             oldest_idx = i;
//         }
//     }
//
//     strncpy(web_session_list[oldest_idx].token, token, sizeof(web_session_list[oldest_idx].token));
//     web_session_list[oldest_idx].expires = now + SESSION_LIFETIME_SEC;
// }
//
//
//
//
// /**
//  * Проверка логина и пароля по списку пользователей
//  */
// static bool check_user_credentials(const char *username, const char *password)
// {
//     unsigned char pass_hash_hex[32];
//
//     calc_password_hash(password, pass_hash_hex, sizeof(pass_hash_hex));
//
//     for (int i = 0; i < MAX_USERS; i++) {
//         if (strcmp(web_users_list.user[i].user, username) == 0 &&
//             memcmp(web_users_list.user[i].pass_hash, pass_hash_hex, 32) == 0) {
//             LOG_INF("Auth success for user '%s'", username);
//             return true;
//             }
//     }
//     LOG_WRN("Auth failed for user '%s'", username);
//     return false;
// }
//
// /**
//  * Проверка токена в списке активных сессий
//  */
// static bool check_token_valid(const char *token)
// {
//     uint32_t now = (uint32_t)time(NULL);
//
//     for (size_t i = 0; i < MAX_SESSIONS; i++) {
//         if (strlen(web_session_list[i].token) == 0)
//             continue;
//
//         if (strcmp(web_session_list[i].token, token) == 0) {
//             if (web_session_list[i].expires > now) {
//                 LOG_INF("Session valid (expires in %u sec)", web_session_list[i].expires - now);
//                 return true;
//             } else {
//                 LOG_WRN("Session expired");
//                 web_session_list[i].token[0] = '\0';
//                 return false;
//             }
//         }
//     }
//
//     LOG_WRN("Token not found in active sessions");
//     return false;
// }
//
//
// int authentication(const struct http_header *headers, size_t header_count, char * token_buf, size_t token_buf_size) {
//
//     LOG_DBG("authentication_check_user");
//     //settings_load_one(web_users_settings, &web_users_list, sizeof(web_users_list));
//
//     const char *token = NULL;
//     bool check_only = (token_buf == NULL || token_buf_size == 0);
//
//     // try to find token in headers
//     for (size_t i = 0; i < header_count; i++) {
//         // check if
//         const char *header_name = headers[i].name;
//         const char *header_value = headers[i].value;
//
//         /* 1️⃣ Cookie: session_token=... */
//         if (strcasecmp(header_name, "Cookie") == 0) {
//             const char *pos = strstr(header_value, "session_token=");
//             if (pos) {
//                 pos += strlen("session_token=");
//                 const char *end = strchr(pos, ';');
//                 size_t len = end ? (size_t)(end - pos) : strlen(pos);
//                 char local_token[SESSION_TOKEN_SIZE];
//                 if (len < sizeof(local_token)) {
//                     strncpy(local_token, pos, len);
//                     local_token[len] = '\0';
//                     if (check_token_valid(local_token)) return 0;
//                 }else {
//                     LOG_WRN("Invalid session token, too long, ignored");
//                 }
//             }
//
//         } else if (strcasecmp(headers[i].name, "Authorization") == 0) {
//             // Альтернатива: "Authorization: Bearer <token>"
//             if (strncasecmp(header_value, "Basic ", 6) == 0) {
//                 /* ----- Basic Auth ----- */
//                 if (check_only)
//                     continue; // В режиме проверки игнорируем Basic
//
//                 const char *encoded = header_value + 6;
//                 uint8_t decoded[256];
//                 size_t olen = 0;
//
//                 if (base64_decode(decoded, sizeof(decoded), &olen, (const uint8_t *)encoded, strlen(encoded)) == 0) {
//                     decoded[MIN(olen, sizeof(decoded) - 1)] = '\0';
//                     char *colon = strchr((char *)decoded, ':');
//                     if (colon) {
//                         *colon = '\0';
//                         const char *username = (char *)decoded;
//                         const char *password = colon + 1;
//
//                         if (check_user_credentials(username, password)) {
//                             char new_token[SESSION_TOKEN_SIZE];
//                             generate_token(username, new_token, sizeof(new_token));
//                             store_session_token(new_token);
//
//                             if (token_buf && token_buf_size > 0) {
//                                 strncpy(token_buf, new_token, token_buf_size - 1);
//                                 token_buf[token_buf_size - 1] = '\0';
//                             }
//                             LOG_INF("New session token created for '%s'", username);
//                             return 0;
//                         }else {
//                             LOG_WRN("Failed to authenticate user '%s'", username);
//                             return -EACCES;
//                         }
//
//                     }
//                 } else {
//                   LOG_WRN("Failed to decode Basic Auth");
//                 }
//             }
//
//             else if (strncasecmp(header_value, "Bearer ", 7) == 0) {
//                 /* ----- Bearer Token ----- */
//                 const char *pos = header_value + 7;
//                 char local_token[SESSION_TOKEN_SIZE];
//                 strncpy(local_token, pos, sizeof(local_token));
//                 local_token[sizeof(local_token) - 1] = '\0';
//
//                 if (check_token_valid(local_token)) return 0;
//             }
//         }
//     }
//
//     return -EACCES;
// }
//
// int add_web_user(const char *username, const char *password) {
//
//     if (username == NULL || password == NULL) {
//         return -EINVAL;
//     }
//
//     bool changed = false;
//     //settings_load_one(web_users_settings, &web_users_list, sizeof(web_users_list));
//     // Ищем первый свободный слот
//     for (int i = 0; i < MAX_USERS; i++) {
//
//         if (strcmp(web_users_list.user[i].user, username) == 0) {
//             LOG_WRN("User '%s' already exists", username);
//             return -EEXIST;
//         }
//
//         if (web_users_list.user[i].user[0] == '\0') {
//             unsigned char pass_hash_hex[MAX_PASS_HASH_LEN];
//             if ( 0 > calc_password_hash(password, pass_hash_hex, sizeof(pass_hash_hex))) {
//                 return -EIO;
//             }
//             strncpy(web_users_list.user[i].user, username, sizeof(web_users_list.user[i].user));
//             web_users_list.user[i].user[sizeof(web_users_list.user[i].user) - 1] = '\0';
//             memcpy(web_users_list.user[i].pass_hash, pass_hash_hex, sizeof(pass_hash_hex));
//             changed = true;
//             break;
//         }
//     }
//
//     if (!changed) {
//         return -ENOSPC;
//     }
//
//     if (update_users(&web_users_list) != 0) {
//         return -EIO;
//     }
//
//     return 0;
// }
//
// int delete_web_user(const char *username) {
//
//     web_users_list.changed = false;
//
//     if (username == NULL) {
//         LOG_ERR("call delete user with username = NULL");
//         return -EINVAL;
//     }
//
//     //count number of users in list
//     int num_users = 0;
//     for (int i = 0; i < MAX_USERS; i++) {
//         if (web_users_list.user[i].user[0] != '\0') {
//             num_users++;
//         }
//     }
//
//     for (int i = 0; i < MAX_USERS; i++) {
//         if (strcmp(web_users_list.user[i].user, username) == 0) {
//             if (num_users == 1) {
//                 LOG_WRN("trying to delete last user");
//                 return -EPERM;
//             }
//             memset(web_users_list.user[i].user, 0, sizeof(web_users_list.user[i].user));
//             memset(web_users_list.user[i].pass_hash, 0, sizeof(web_users_list.user[i].pass_hash));
//             web_users_list.changed = true;
//             break;
//         }
//     }
//
//     if (web_users_list.changed == true) {
//         if (update_users(&web_users_list) != 0) {
//             LOG_ERR("failed to update users");
//             return -EIO;
//         }
//     }else {
//         LOG_ERR("no users to delete");
//         return -ENOENT;
//     }
//     return 0;
// }