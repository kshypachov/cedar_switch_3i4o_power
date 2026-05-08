//
// Created by Kirill Shypachov on 16.10.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_AUTHENTICATION_H
#define CEDAR_SWITCH_3IN3OUT_POWER_AUTHENTICATION_H

#include <zephyr/net/http/server.h>
#include "../settings_topics.h"
#define SESSION_TOKEN_SIZE 128

void authentication_init(void);

/**
 *
 * @param headers must include Authorization header with Basic or Bearer scheme, or Cookie header with session=<session token>
 * @param header_count
 * @param token_buf buffer for writing generated session token if basic authentication success. If NULL, function just validate session token from headers.
 * @param token_buf_size
 * @return 0 or 0 <  if success, return < 0 - fail
 */
int authentication(const struct http_header *headers, size_t header_count, char * token_buf, size_t token_buf_size);

int get_web_users (web_users_list_t *users_list);

int add_web_user(const char *username, const char *password);

int delete_web_user(const char *username);

#endif //CEDAR_SWITCH_3IN3OUT_POWER_AUTHENTICATION_H