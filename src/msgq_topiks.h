//
// Created by Kirill Shypachov on 22.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_MSGQ_TOPIKS_H
#define CEDAR_SWITCH_3IN3OUT_POWER_MSGQ_TOPIKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "global_var.h"


typedef struct {
    char     topic[mqtt_topik_max_len];
    char     payload[mqtt_payload_max_len];
    uint8_t  qos;      // 0/1/2
    bool     retain;
}mqtt_msg_t;

extern struct k_msgq MQTT_MSGQ_TX; // Q for sending data from device to mqtt server
extern struct k_msgq MQTT_MSGQ_RX; // Q for recieve message from mqtt server

#endif //CEDAR_SWITCH_3IN3OUT_POWER_MSGQ_TOPIKS_H