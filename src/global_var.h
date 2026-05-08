//
// Created by Kirill Shypachov on 18.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_GLOBAL_VAR_H
#define CEDAR_SWITCH_3IN3OUT_POWER_GLOBAL_VAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "events/events.h"

#define base_web_ui_fs_path "/lfs/log"

#define MQTT_UNIC_IDENTIFIER_LENGTH     32
#define hw_ver                          "1.0"
#define sw_ver                          "1.0"

#define device_input_count              3
#define device_relay_count              4

#define mqtt_topik_max_len              128
#define mqtt_payload_max_len            700

#endif //CEDAR_SWITCH_3IN3OUT_POWER_GLOBAL_VAR_H