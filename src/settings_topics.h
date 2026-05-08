//
// Created by Kirill Shypachov on 14.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_SETTINGS_TOPICS_H
#define CEDAR_SWITCH_3IN3OUT_POWER_SETTINGS_TOPICS_H
#include <stdbool.h>

#define def_relays_state_enable_settings_topik "/settings/relays/default/enabled"
#define def_relays1_state_settings_topik      "/settings/relays/default/1"
#define def_relays2_state_settings_topik      "/settings/relays/default/2"
#define def_relays3_state_settings_topik      "/settings/relays/default/3"
#define def_relays4_state_settings_topik      "/settings/relays/default/4"

typedef struct  {
    bool enabled;
    bool relay1;
    bool relay2;
    bool relay3;
    bool relay4;
} relays_def_state;


#define mqtt_enabled_settings "/settings/mqtt/enabled"
#define mqtt_secure_settings  "/settings/mqtt/secure"
#define mqtt_host_settings    "/settings/mqtt/host"
#define mqtt_port_settings    "/settings/mqtt/port"
#define mqtt_user_settings    "/settings/mqtt/user"
#define mqtt_pass_settings    "/settings/mqtt/pass"

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