//
// Created by Kirill Shypachov on 22.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_HA_MQTT_ADAPTER_H
#define CEDAR_SWITCH_3IN3OUT_POWER_HA_MQTT_ADAPTER_H

#include <zephyr/net/mqtt.h>


int ha_mqtt_init(void);
int ha_mqtt_subscribe_topiks(const struct mqtt_client *client);
const char *ha_mqtt_get_device_id(void);
int ha_send_data_from_q_to_mqtt( struct mqtt_client *client, struct mqtt_publish_param *pub_param);



#endif //CEDAR_SWITCH_3IN3OUT_POWER_HA_MQTT_ADAPTER_H