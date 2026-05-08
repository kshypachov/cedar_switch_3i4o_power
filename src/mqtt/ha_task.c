//
// Created by Kirill Shypachov on 22.09.2025.
//

#include "ha_task.h"

#include "../zbus_topics.h"
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/socket_service.h>
#include <zephyr/settings/settings.h>
#include <zephyr/net/sntp.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <arpa/inet.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/random/random.h>

#include "ha_mqtt_adapter.h"
#include "../settings_topics.h"

#include "ha_mqtt_callback.h"
#include "ha_mqtt_helpers.h"
#include "../msgq_topiks.h"

#define HA_MQTT_TASK_PRIORITY   5
#define DEVICE_SENSORS_COUNT    6

#define UPDATE_PERIOD_SEC       (ha_mqtt_keepalive_sec - 100)

LOG_MODULE_REGISTER(ha_task);

K_THREAD_STACK_DEFINE(ha_task_stack, 3100);
static struct k_thread ha_task_thread_data;

uint32_t next_update_input = 0;
uint32_t next_update_output = 0;

typedef struct {
    int type;
    int index;
}device_t;


/* Вспомогательная функция: достаём конкретный бит (нумерация с 0) */
static inline uint8_t get_bit(uint32_t state, uint8_t index) {
    return (state >> (index)) & 0x1;
}


int ha_mqtt_register_device(struct mqtt_client *const client) {

    /*device structure
 * device type - index (if index -1 it s mean do not add index to topics or displayed names)
 * input    1
 * input    2
 * input    3
 *
 * relay    1
 * relay    2
 * relay    3
 *
 * AC voltage -1
 * AC current -1
 * AC apparet pow -1
 */

    while (1) {


    }
}


void ha_mqtt_task(void *p1, void *p2, void *p3) {

    LOG_INF("Start HA MQTT task");
    mqtt_msg_t          mqtt_message = {0};
    inputs_msg_t        inputs_state = {0}, inputs_state_old = { 0, 0b11111111};
    outputs_msg_t       outputs_state = {0}, outputs_state_old = {0, 0b11111111};
    mqtt_status_msg_t   mqtt_ha_status = {0};
    int                 rc = 0;

    device_t device[DEVICE_SENSORS_COUNT] = {
        {INPUT_SENSOR, 1,},
        {INPUT_SENSOR, 2,},
        {INPUT_SENSOR, 3,},
        {OUTPUT_DEVICE, 1},
        {OUTPUT_DEVICE, 2},
        {OUTPUT_DEVICE, 3}
    };

    while (1) {

        ha_mqtt_init();
        //wait opening mqtt connection
        do {
            LOG_INF("Wait 2 seconds for mqtt connection");
            k_sleep(K_SECONDS(2));
            zbus_chan_read(&mqtt_stat_zbus_topik, &mqtt_ha_status, K_NO_WAIT);
        }while (!mqtt_ha_status.connected);

        //k_sleep(K_SECONDS(10));

        LOG_INF("MQTT connected");
        LOG_INF("Start register device in HA");
        //Generate registration data for Home Assistant
        for (int i = 0; i < DEVICE_SENSORS_COUNT; i++) {
            LOG_INF("Generate registration topik for device number: %d", i);
            get_config_topik_string(mqtt_message.topic, sizeof(mqtt_message.topic), device[i].type, device[i].index);
            LOG_DBG("topic: %s", mqtt_message.topic);
            LOG_INF("Generate registration payload for device number: %d", i);
            get_config_payload_string(mqtt_message.payload, sizeof(mqtt_message.payload), device[i].type, device[i].index);
            LOG_DBG("payload: %s", mqtt_message.payload);
            mqtt_message.qos = MQTT_QOS_1_AT_LEAST_ONCE;
            mqtt_message.retain = 1;
            LOG_INF("Put registratin data into mqtt tx mess q");
            k_msgq_put(&MQTT_MSGQ_TX, &mqtt_message, K_FOREVER);
            k_sleep(K_SECONDS(1));

            zbus_chan_read(&mqtt_stat_zbus_topik, &mqtt_ha_status, K_NO_WAIT);
            if (!mqtt_ha_status.connected) {
                LOG_INF("HA MQTT connection is not active. Exit HA registration loop. Will send registration data again");
                break;
            }
        }
        LOG_INF("Register device in HA done");

        LOG_INF("Start send initial data to HA");
        while (1) {
            uint32_t now = k_uptime_seconds();
            // read current mqtt conection state
            zbus_chan_read(&mqtt_stat_zbus_topik, &mqtt_ha_status, K_NO_WAIT);
            if (!mqtt_ha_status.connected) {
                LOG_INF("HA MQTT connection is not active. Exit HA evet loop. Will send registration data again");
                break;
            }

            //read inputs state and update if state changed or pereodic timeout
            zbus_chan_read(&inputs_zbus_topik, &inputs_state, K_NO_WAIT);
            // if time fore next update - invert state of inputs_state_old for initiate updating statuses for all inputs
            if (now > next_update_input) {
                inputs_state_old.state = ~inputs_state.state;
                next_update_input = now + UPDATE_PERIOD_SEC;
            }

            if ((inputs_state.state != inputs_state_old.state) ) {

                // iterate all device list
                for (int i = 0; i < DEVICE_SENSORS_COUNT; i++) {
                    //if (device[i].type == INPUT_SENSOR) {
                    if (device[i].type == INPUT_SENSOR &&
                        get_bit(inputs_state.state,     device[i].index - 1) !=
                        get_bit(inputs_state_old.state, device[i].index - 1)) {

                        mqtt_message.qos = MQTT_QOS_1_AT_LEAST_ONCE;
                        mqtt_message.retain = 0;

                        rc = get_status_topik_string (mqtt_message.topic, sizeof(mqtt_message.topic), INPUT_SENSOR, device[i].index);
                        if (rc != 0) {
                            LOG_ERR("get_status_topik_string() = %d, dev_type = INPUT_SENSOR, dev_indev = %d", rc, device[i].index);
                        }
                        // transform input state to json like {status: ON}
                        // inputs_state.state = 0b00100100
                        // device[i].index = 2
                        // 0b00100100 >> 2 = 0b00001001
                        // 0b00001001 & 1 = 0b00000001
                        generate_sensor_status_payload_JSON(mqtt_message.payload, mqtt_payload_max_len, INPUT_SENSOR, device[i].index, (inputs_state.state >> (device[i].index - 1)) & 1);
                        k_msgq_put(&MQTT_MSGQ_TX, &mqtt_message, K_FOREVER);

                    }
                }
                inputs_state_old = inputs_state;
            }

            //write parsing message from MQTT message Q to io zbus
            if ((k_msgq_num_used_get(&MQTT_MSGQ_RX) > 0) && (k_msgq_get(&MQTT_MSGQ_RX, &mqtt_message, K_NO_WAIT) == 0 )) {
                mqtt_ha_dev_type_t tmp_dev_type;
                uint8_t tmp_sensor_number;
                uint8_t tmp_state;

                rc = parse_incoming_message(
                        mqtt_message.payload,
                        sizeof(mqtt_message.payload),
                        mqtt_message.topic,
                        sizeof(mqtt_message.topic),
                        &tmp_dev_type,
                        &tmp_sensor_number,
                        &tmp_state);
                if (rc != 0) {
                    LOG_INF("MQTT message parse error");
                }else {
                    if (tmp_dev_type == OUTPUT_DEVICE) {
                        outputs_state.seq ++;

                        uint8_t mask = 1u << (tmp_sensor_number - 1);
                        if (tmp_state) {
                            outputs_state.state |= mask;
                        } else {
                            outputs_state.state &= ~mask;
                        }
                        zbus_chan_pub(&outputs_zbus_topik, &outputs_state, K_NO_WAIT);
                    }else {
                        LOG_ERR("Wrong device type");
                    }
                }
            }




            //read outputs state and update if state changed or pereodic timeout
            zbus_chan_read(&outputs_zbus_topik, &outputs_state, K_NO_WAIT);
            // if time fore next update - invert state of inputs_state_old for initiate updating statuses for all outputs
            if (now > next_update_output) {
                outputs_state_old.state = ~outputs_state.state;
                next_update_output = now + UPDATE_PERIOD_SEC;
            }

            if ((outputs_state.state != outputs_state_old.state)) {
                // iterate all device list
                for (int i = 0; i < DEVICE_SENSORS_COUNT; i++) {
                    //if (device[i].type == OUTPUT_DEVICE && (((outputs_state.state >> (device[i].index - 1)) & 1) != (outputs_state_old.state >> (device[i].index - 1)) & 1) {
                    if (device[i].type == OUTPUT_DEVICE &&
                        get_bit(outputs_state.state,     device[i].index - 1) !=
                        get_bit(outputs_state_old.state, device[i].index - 1)) {

                        mqtt_message.qos = MQTT_QOS_1_AT_LEAST_ONCE;
                        mqtt_message.retain = 0;

                        rc = get_status_topik_string (mqtt_message.topic, sizeof(mqtt_message.topic), OUTPUT_DEVICE, device[i].index);
                        if (rc != 0) {
                            LOG_ERR("get_status_topik_string() = %d, dev_type = OUTPUT_DEVICE, dev_indev = %d", rc, device[i].index);
                        }
                        // transform input state to json like {status: ON}
                        // inputs_state.state = 0b00100100
                        // device[i].index = 2
                        // 0b00100100 >> 2 = 0b00001001
                        // 0b00001001 & 1 = 0b00000001
                        generate_sensor_status_payload_JSON(mqtt_message.payload, mqtt_payload_max_len, OUTPUT_DEVICE, device[i].index, (outputs_state.state >> (device[i].index - 1)) & 1);
                        k_msgq_put(&MQTT_MSGQ_TX, &mqtt_message, K_FOREVER);
                    }
                }

                outputs_state_old = outputs_state;
            }


            //Get data from sensors and set to MQTT topiks
            k_sleep(K_MSEC(100));
        }

        k_sleep(K_SECONDS(1));
    }

}

void start_ha_mqtt_task(void) {

    LOG_INF("Starting HA MQTT task");
    k_tid_t tid = k_thread_create(&ha_task_thread_data,
        ha_task_stack,
        K_THREAD_STACK_SIZEOF(ha_task_stack),
        ha_mqtt_task,
        NULL, NULL, NULL,
        HA_MQTT_TASK_PRIORITY,
        0,
        K_NO_WAIT);

    k_thread_name_set(tid, "ha_task");

}