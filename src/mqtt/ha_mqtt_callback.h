//
// Created by Kirill Shypachov on 20.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_MQTT_CALLBACK_H
#define CEDAR_SWITCH_3IN3OUT_POWER_MQTT_CALLBACK_H

#include <zephyr/net/mqtt.h>

void mqtt_evt_handler(struct mqtt_client *const client, const struct mqtt_evt *evt);

#endif //CEDAR_SWITCH_3IN3OUT_POWER_MQTT_CALLBACK_H