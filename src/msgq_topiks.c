//
// Created by Kirill Shypachov on 22.09.2025.
//

#include "msgq_topiks.h"
#include <zephyr/kernel.h>

K_MSGQ_DEFINE(MQTT_MSGQ_TX, sizeof(mqtt_msg_t), 16, 4);
K_MSGQ_DEFINE(MQTT_MSGQ_RX, sizeof(mqtt_msg_t), 16, 4);