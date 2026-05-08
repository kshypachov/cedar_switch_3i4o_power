//
// Created by Kirill Shypachov on 15.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_ZBUS_TOPICS_H
#define CEDAR_SWITCH_3IN3OUT_POWER_ZBUS_TOPICS_H
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <stdbool.h>
#include "events/events.h"

#define outputs_zbus_topik outputs_topic
#define inputs_zbus_topik inputs_topic
#define mqtt_stat_zbus_topik mqtt_status_topic
#define energy_mon_zbus_topik energy_mon_topic

ZBUS_CHAN_DECLARE(outputs_zbus_topik);
ZBUS_CHAN_DECLARE(inputs_zbus_topik);
ZBUS_CHAN_DECLARE(mqtt_stat_zbus_topik);
ZBUS_CHAN_DECLARE(energy_mon_zbus_topik);

ZBUS_OBS_DECLARE(io_sub);
ZBUS_OBS_DECLARE(matter_sub);
//ZBUS_MSG_SUBSCRIBER_DEFINE(matter_sub);

ZBUS_CHAN_DECLARE(io_events);


//structure for relay state exchange
typedef struct {
    uint32_t seq;   /* уникальный номер или счётчик сообщения */
    uint8_t state;  /* биты реле */
} outputs_msg_t;

typedef struct {
    uint32_t seq;  /**/
    uint8_t state;  /**/
} inputs_msg_t;

typedef struct  {
    uint64_t seq;
    bool enabled;
    bool connected;
}mqtt_status_msg_t;

typedef struct {
    float energy_kWh_total;
    float voltage;
    float current;
    float frequency;
    float power_factor;
}enegry_mon_t;

typedef struct {
    enum event_direction event_direction;
    enum io_events io_event;                // Type of event
    bool bool_data;                       //Data for transfer to another tasks 0 or 1
} io_event_data_t;

#endif //CEDAR_SWITCH_3IN3OUT_POWER_ZBUS_TOPICS_H