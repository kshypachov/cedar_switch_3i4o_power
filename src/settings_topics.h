//
// Created by Kirill Shypachov on 14.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_SETTINGS_TOPICS_H
#define CEDAR_SWITCH_3IN3OUT_POWER_SETTINGS_TOPICS_H
#include <stdbool.h>

typedef struct {
    bool changed;
    bool enabled;
    bool secure;
    char host[128];
    uint16_t port;
    char user[128];
    char pass[128];
} mqtt_settings_t;

#define web_users_settings "/settings/web/users"
#define MAX_USERS 3
#define MAX_USERNAME_LEN 32
#define MAX_PASS_HASH_LEN 32
#define MAX_PASS_TEXT_LEN MAX_USERNAME_LEN


typedef struct {
    char user[MAX_USERNAME_LEN];
    uint8_t pass_hash[MAX_PASS_HASH_LEN];
}web_user_t;

typedef struct {
    bool changed;
    web_user_t user[MAX_USERS];
}web_users_list_t;

#define energy_stat_file "energy.stat"

#endif //CEDAR_SWITCH_3IN3OUT_POWER_SETTINGS_TOPICS_H