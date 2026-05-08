//
// Created by Kirill Shypachov on 20.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_MQTT_HELPERS_H
#define CEDAR_SWITCH_3IN3OUT_POWER_MQTT_HELPERS_H

#include <zephyr/net/mqtt.h>
#include "../global_var.h"

typedef enum{
    INPUT_SENSOR			= 1,
    OUTPUT_DEVICE			= 2,
    ENERGY_SENSOR			= 3,
    POWER_SENSOR			= 4,
    VOLTAGE_SENSOR			= 5,
    POWER_FACTOR_SENSOR		= 6,
    CURRENT_SENSOR 			= 7,
    APPARENT_POWER_SENSOR	= 8,
    VOLTAGE_DIAGNOSTIC_BATT_SENSOR	= 9,
    VOLTAGE_DIAGNOSTIC_POW_SUPL_SENSOR	= 10,
}mqtt_ha_dev_type_t;

// === Очередь публикаций (topic+payload) ===
#define MQTT_TOPIC_MAX     mqtt_topik_max_len
#define MQTT_PAYLOAD_MAX   mqtt_payload_max_len
#define MQTT_MSGQ_PUB_NAME mqtt_ha_pubq

#define ha_mqtt_keepalive_sec			600



int set_device_id(const uint8_t* id, unsigned const int id_len);
const char *get_device_id(void);
int set_device_conf_ip(const char *ip_str);
const char *get_device_conf_ip(void);
int get_status_topik_string (char * buff, size_t buff_size, mqtt_ha_dev_type_t dev_type, uint8_t obj_number);
int get_config_topik_string (char * buff, uint32_t buff_len, mqtt_ha_dev_type_t dev_type, uint8_t obj_number);
int get_config_payload_string( char * payload, size_t payload_len, mqtt_ha_dev_type_t payload_type, uint8_t obj_number);
int generate_command_topik_for_subscrabe(char * topik, uint32_t topik_len, mqtt_ha_dev_type_t dev_type, uint8_t sensor_number);
int generate_sensor_status_payload_JSON(char * payload, size_t payload_size, mqtt_ha_dev_type_t dev_type, uint8_t sensor_number, uint8_t state);
int parse_incoming_message (const char * payload, size_t payload_size, const char * topic, size_t topic_size, mqtt_ha_dev_type_t * dev_type, uint8_t * sensor_number, uint8_t * state);

#endif //CEDAR_SWITCH_3IN3OUT_POWER_MQTT_HELPERS_H