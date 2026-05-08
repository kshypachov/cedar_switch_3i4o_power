// //
// // Created by Kirill Shypachov on 20.09.2025.
// //
//
// #include "ha_mqtt_helpers.h"
// #include "../global_var.h"
// #include <errno.h>
// #include <arpa/inet.h>
// #include <strings.h>
//
// #define ha_universal_config_topik_template		"%s/%s/%s_%s/%s%u/config"
// #define ha_universal_status_topik_template		"%s_%s/%s%u/state"
// #define ha_io_template							"{\"%s%u\" : \"%s\"}"
//
//
// #define sensor_state_on						"ON"
// #define sensor_state_off					"OFF"
//
// #define dev_system						"cedar"
// #define dev_common_name					"CedarSwitch"
// #define dev_manufacturer_name			"Manufacturer"
// #define dev_model_name					"CedarSwitch-3R3I-POWER"
//
// #define component_sensor				"sensor"
// #define component_binary_sensor			"binary_sensor"
// #define component_switch				"switch"
//
// #define component_input					"input"
// #define component_input_name			"Вхід"
//
// #define dev_class_switch				component_switch
// #define dev_class_switch_name			"Перемикач"
//
// #define component_battery				"battery"
// #define component_battery_name  		"Батарея"
//
// #define component_power_supply			"power_supply"
// #define component_power_supply_name 	"Джерело живлення"
//
// #define dev_class_energy				"energy"
// #define dev_class_energy_name   	    "Енергія"
// #define dev_class_energy_state			"\"state_class\": \"total_increasing\",\n"
// #define dev_class_energy_unit_of_measurement	"kWh"
//
// #define dev_class_voltage				"voltage"
// #define dev_class_voltage_name			"Напруга"
// #define dev_class_voltage_unit_of_measurement	"V"
//
// #define dev_class_power					"power"
// #define dev_class_power_name			"Активна потужність"
// #define dev_class_power_unit_of_measurement	"W"
//
// #define dev_class_apparent_power		"apparent_power"
// #define dev_class_apparent_power_name	"Повна потужність"
// #define dev_class_apparent_power_unit_of_measurement	"VA"
//
// #define dev_class_power_factor			"power_factor"
// #define dev_class_power_factor_name	    "Коефіцієнт потужності"
// #define dev_class_power_factor_unit_of_measurement "%"
//
// #define dev_class_current				"current"
// #define dev_class_current_name			"Струм"
// #define dev_class_current_unit_of_measurement	"A"
//
//
//
// #define dev_hw_ver						hw_ver
// #define dev_sw_ver						sw_ver
//
// LOG_MODULE_REGISTER(ha_mqtt_helpers);
//
// static char ha_mqtt_dev_identifier[MQTT_UNIC_IDENTIFIER_LENGTH] = {0};
// static char	dev_conf_ip [INET6_ADDRSTRLEN]	= {0}; //IPv4 or IPv6
// char home_assistant_prefix[] = {"homeassistant"};
//
// const char ha_universal_conf_template[]={
// 	"{\n"
// 			"\t\"device_class\" :\"%s\",\n"
// 			"\t\"expire_after\" : %u ,\n"
// 			"\t\"state_topic\" :\"%s_%s/%s%u/state\",\n"
// 			"\t\"value_template\":\"{{ value_json.%s%u }}\",\n"
// 			"\t%s"
// 			"\t\"name\":\"%s\",\n"
// 			"\t\"unique_id\":\"%s_%s_%s%u\",\n"
// 			"\t\"unit_of_measurement\": \"%s\",\n"
// 			"\t\"device\":{\n"
// 					"\t\t\"identifiers\":[\"%s_%s\"],\n"
// 					"\t\t\"name\":\"%s\",\n"
// 					"\t\t\"model\":\"%s\",\n"
// 					"\t\t\"manufacturer\":\"%s\",\n"
// 					"\t\t\"hw_version\" : \"%s\",\n"
// 					"\t\t\"sw_version\" : \"%s\",\n"
// 					"\t\t\"configuration_url\" : \"http://%s\"\n"
// 			"\t}\n"
// 	"}\n"
// };
//
// #define ha_universal_template_command_topik_part "\"command_topic\" : \"%s_%s/%s%s/set\",\n"
// #define universal_control_topik_template "%s_%s/%s%u/set" // "[devModel]_[uid(mac)]/[componentName][componentNumber]/[set]"
//
//
//
// int set_device_id(const uint8_t* id, unsigned const int id_len){
//
//     if (id == NULL || id_len == 0) {
//         return -EINVAL;
//     }
//
//     /* Нужно 2 символа на байт + 1 под '\0' */
//     const size_t need = id_len * 2u + 1u;
//     if (need > sizeof(ha_mqtt_dev_identifier)) {
//         return -ENOSPC;
//     }
//
//     // Проходим по каждому байту идентификатора
//     for (unsigned int i = 0; i < id_len; i++) {
//         // Конвертируем каждый байт в два символа и добавляем в строку
//         sprintf(&ha_mqtt_dev_identifier[i * 2], "%02X", id[i]);
//     }
//     return 0;
// }
//
// const char *get_device_id(void)
// /* Возвращает указатель на постоянную строку с ID (или NULL, если не установлен). */
// {
//     /* k_mutex_lock(&dev_id_lock, K_FOREVER); */
//     bool empty = (ha_mqtt_dev_identifier[0] == '\0');
//     /* k_mutex_unlock(&dev_id_lock); */
//     return empty ? NULL : ha_mqtt_dev_identifier;
// }
//
// // валидируем IPv4/IPv6 и только потом сохраняем */
// int set_device_conf_ip(const char *ip_str)
// {
//     if (ip_str == NULL) {
//         return -EINVAL;
//     }
//
//     /* Проверим, валиден ли IPv4 или IPv6 */
//     struct in_addr  v4;
//     struct in6_addr v6;
//
//     int ok_v4 = inet_pton(AF_INET,  ip_str, &v4) == 1;
//     int ok_v6 = inet_pton(AF_INET6, ip_str, &v6) == 1;
//
//     if (!ok_v4 && !ok_v6) {
//         return -EINVAL; /* не распознали адрес */
//     }
//
//     const int n = snprintf(dev_conf_ip, sizeof(dev_conf_ip), "%s", ip_str);
//     if (n < 0) {
//         return n;
//     }
//     if (n >= (int)sizeof(dev_conf_ip)) {
//         return -ENOSPC;
//     }
//     return 0;
// }
//
// const char *get_device_conf_ip(void)
// {
//     return dev_conf_ip[0] ? dev_conf_ip : NULL;
// }
//
// int get_status_topik_string (char * buff, size_t buff_size, mqtt_ha_dev_type_t dev_type, uint8_t obj_number) {
//
// 	strcpy(buff, "");
// 	switch (dev_type) {
// 		case INPUT_SENSOR:
// 			snprintf(buff, buff_size, ha_universal_status_topik_template, dev_system, ha_mqtt_dev_identifier,
// 				component_input, obj_number);
//
// 			break;
//
// 		case OUTPUT_DEVICE:
// 			snprintf(buff, buff_size, ha_universal_status_topik_template, dev_system, ha_mqtt_dev_identifier,
// 				component_switch, obj_number);
//
// 			break;
//
// 		default:
// 			return -EINVAL;
// 	}
//
// 	return 0;
// }
//
// int get_config_topik_string (char * buff, uint32_t buff_len, mqtt_ha_dev_type_t dev_type, uint8_t obj_number){
//
// 	strcpy(buff, "");
// 	switch (dev_type) {//%s/%s/%s_%s/%s%u/config
// 		case INPUT_SENSOR:
// 			snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_binary_sensor, dev_system, ha_mqtt_dev_identifier, component_input, obj_number);
// 			break;
// 		case OUTPUT_DEVICE:
// 			snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_switch, dev_system, ha_mqtt_dev_identifier, dev_class_switch, obj_number);
// 			break;
// 	    case VOLTAGE_DIAGNOSTIC_BATT_SENSOR:
// 			snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_sensor, dev_system, ha_mqtt_dev_identifier, component_battery, obj_number);
// 			break;
// 	    case VOLTAGE_DIAGNOSTIC_POW_SUPL_SENSOR:
// 	    	snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_sensor, dev_system, ha_mqtt_dev_identifier, component_power_supply, obj_number);
// 	    	break;
// 		case ENERGY_SENSOR:
// 			snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_sensor, dev_system, ha_mqtt_dev_identifier, dev_class_energy, obj_number);
// 			break;
// 		case VOLTAGE_SENSOR:
// 			snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_sensor, dev_system, ha_mqtt_dev_identifier, dev_class_voltage, obj_number);
// 			break;
// 		case POWER_SENSOR:
// 			snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_sensor, dev_system, ha_mqtt_dev_identifier, dev_class_power, obj_number);
// 			break;
// 		case APPARENT_POWER_SENSOR:
// 			snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_sensor, dev_system, ha_mqtt_dev_identifier, dev_class_apparent_power, obj_number);
// 			break;
// 		case POWER_FACTOR_SENSOR:
// 			snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_sensor, dev_system, ha_mqtt_dev_identifier, dev_class_power_factor, obj_number);
// 			break;
// 		case CURRENT_SENSOR:
// 			snprintf(buff, buff_len, ha_universal_config_topik_template, home_assistant_prefix, component_sensor, dev_system, ha_mqtt_dev_identifier, dev_class_current, obj_number);
// 			break;
//
// 		default:
// 			return -1;
// 			break;
// 	}
// 	return 0;
// }
//
// int get_config_payload_string( char * payload, const size_t payload_len, const mqtt_ha_dev_type_t payload_type, const uint8_t obj_number){
//
// 	uint8_t len = 0;
// 	char name[MQTT_TOPIC_MAX] = {0};
// 	char command_topik[MQTT_TOPIC_MAX] = {0};
// 	memset(payload, 0, payload_len * sizeof(char));
//
// 	switch (payload_type) {
//
// 		case INPUT_SENSOR:
// 			if (obj_number == 0) { // if object == -1 do not add number to name which will displayed in Home Assistant
// 				snprintf(name, MQTT_TOPIC_MAX, "%s", component_input_name);
// 			}else {
// 				snprintf(name, MQTT_TOPIC_MAX, "%s %u", component_input_name, obj_number);
// 			}
//
// 			len = snprintf(payload, payload_len, ha_universal_conf_template, dev_class_power, ha_mqtt_keepalive_sec, dev_system, \
// 				ha_mqtt_dev_identifier, component_input, obj_number, component_input, obj_number, "\n", \
// 				name, dev_system, ha_mqtt_dev_identifier, \
// 				component_input, obj_number,"",dev_system, ha_mqtt_dev_identifier, dev_common_name, dev_model_name, \
// 				dev_manufacturer_name, dev_hw_ver, dev_sw_ver, dev_conf_ip);
// 			return len;
// 			break;
//
// 		case OUTPUT_DEVICE:
// 			if (obj_number == 0) { // if object == -1 do not add number to name which will displayed in Home Assistant
// 				snprintf(name, MQTT_TOPIC_MAX, "%s", dev_class_switch_name);
// 				snprintf(command_topik, MQTT_TOPIC_MAX, ha_universal_template_command_topik_part, dev_system, ha_mqtt_dev_identifier, dev_class_switch, "");
// 			}else {
// 				char digit_chr[5] = {0};
// 				snprintf(digit_chr, sizeof(digit_chr), "%u", obj_number);
// 				snprintf(name, MQTT_TOPIC_MAX, "%s %u", dev_class_switch_name, obj_number);
// 				snprintf(command_topik, MQTT_TOPIC_MAX, ha_universal_template_command_topik_part , dev_system, ha_mqtt_dev_identifier, dev_class_switch, digit_chr);
// 			}
//
// 			len = snprintf(payload, payload_len, ha_universal_conf_template, dev_class_switch, ha_mqtt_keepalive_sec, dev_system, \
// 				ha_mqtt_dev_identifier, component_switch, obj_number, component_switch, obj_number, command_topik, \
// 				name, dev_system, ha_mqtt_dev_identifier, \
// 				component_switch, obj_number,"",dev_system, ha_mqtt_dev_identifier, dev_common_name, dev_model_name, \
// 				dev_manufacturer_name, dev_hw_ver, dev_sw_ver, dev_conf_ip);
//
// 			return len;
// 			break;
//
// 		default:
// 			return -1;
// 			break;
// 	}
// }
//
//
// int generate_command_topik_for_subscrabe(char * topik, uint32_t topik_len, mqtt_ha_dev_type_t dev_type, uint8_t sensor_number){
//
// 	switch(dev_type) {
// 		case OUTPUT_DEVICE:
// 			return snprintf(topik, topik_len, universal_control_topik_template, dev_system, ha_mqtt_dev_identifier, dev_class_switch, sensor_number );
// 			break;
//
// 		default:
// 			return -1;
// 			break;
// 	}
// }
//
// int generate_sensor_status_payload_JSON(char * payload, size_t payload_size, mqtt_ha_dev_type_t dev_type, uint8_t sensor_number, uint8_t state){
//
// 	char * sens_name = NULL;
// 	switch (dev_type) {
// 		case INPUT_SENSOR:
// 			sens_name = (char *)component_input;
// 			break;
// 		case OUTPUT_DEVICE:
// 			sens_name = (char *)dev_class_switch;
// 			break;
// 		default:
// 			break;
// 	}
//
// 	if (state){
// 		return snprintf(payload, payload_size, ha_io_template, sens_name, sensor_number, sensor_state_on);
// 	}else{
// 		return snprintf(payload, payload_size, ha_io_template, sens_name, sensor_number, sensor_state_off);
// 	}
// }
//
// static int get_sensor_type_from_str(const char * sensor_type_str, mqtt_ha_dev_type_t * dev_type) {
//
// 	if (strcmp(sensor_type_str, "input") == 0) {
// 		*dev_type = INPUT_SENSOR;
// 		return 0;
// 	}else if (strcmp(sensor_type_str, "switch") == 0) {
// 		*dev_type = OUTPUT_DEVICE;
// 		return 0;
// 	}else {
// 		return -1;
// 	}
// }
//
// static int get_sensor_state_from_str(const char * sensor_state_str, uint8_t * state) {
//
// 	if (!sensor_state_str || !state) {
// 		return -1; // неверные аргументы
// 	}
//
// 	// Истинные значения: "on", "ON", "On", "1"
// 	if (strcasecmp(sensor_state_str, "on") == 0 ||
// 		strcmp(sensor_state_str, "1") == 0) {
// 		*state = 1;
// 		return 0;
// 		}
//
// 	// Ложные значения: "off", "0"
// 	if (strcasecmp(sensor_state_str, "off") == 0 ||
// 		strcmp(sensor_state_str, "0") == 0) {
// 		*state = 0;
// 		return 0;
// 		}
//
// 	return -1; // неизвестное состояние
// }
//
// int parse_incoming_message (const char * payload, size_t payload_size, const char * topic, size_t topic_size, mqtt_ha_dev_type_t * dev_type, uint8_t * sensor_number, uint8_t * state_num) {
//
// 	char devtype_str[32];
// //	char json_key[32];
// 	char state_str[8];
// 	char index_str[8];
// 	int consumed = 0;
// 	int rc = 0;
//
// 	// Маска для топика: %[^/]/%[^0-9]%d/set "%31[a-zA-Z]"
// 	// %*[^/] — проигнорировать часть до последнего сегмента
// 	rc = sscanf(topic, "%*[^/]/%31[a-zA-Z]%7[0-9]/set%n", devtype_str, index_str, &consumed);
// 	if ( rc != 2 || topic[consumed] != '\0') {
// 		LOG_ERR("Parsing topik %s is unsucces", topic);
// 		return -1;
// 	}
//
// 	/* Конвертируем index_str -> int через strtol с проверками */
// 	errno = 0;
// 	char *end = NULL;
// 	long v = strtol(index_str, &end, 10);
// 	if (errno == ERANGE || end == index_str || *end != '\0' || v < 0 || v > INT_MAX) {
// 		/* LOG_ERR("Bad index '%s' in topic %s", index_str, topic); */
// 		return -1;
// 	}
//
// 	int index = (int) v;
//
// 	// // Маска для JSON: {"ключ":"значение"}
// 	// if (sscanf(payload, "{ \"%31[a-zA-Z0-9_]\" : \"%7[a-zA-Z0-9]\" }", json_key, state_str) != 2) {
// 	// 	LOG_ERR("Parsing payload &s is unsucces", payload);
// 	// 	return -1;
// 	// }
//
// 	if (sscanf(payload, "%7[a-zA-Z0-9]", state_str) != 1) {
// 		LOG_ERR("Parsing payload %s is unsucces", payload);
// 		return -1;
// 	}
//
// 	// Проверка совпадений
// 	// char expected[32];
// 	// snprintf(expected, sizeof(expected), "%s%d", devtype_str, index);
// 	//
// 	// if (strcmp(json_key, expected) != 0) {
// 	// 	printf("Несовпадение ключа: ожидалось %s, а пришло %s\n", expected, json_key);
// 	// 	return -1;
// 	// }
//
// 	if (get_sensor_type_from_str(devtype_str, dev_type) != 0) {
// 		LOG_ERR("Parsing device type %s is unsucces", payload);
// 		return -1;
// 	}
//
// 	if (get_sensor_state_from_str(state_str , state_num) != 0) {
// 		LOG_ERR("Parsing device state %s is unsucces", state_str);
// 		return -1;
// 	}
//
// 	*sensor_number = index;
//
// 	return 0;
// }
