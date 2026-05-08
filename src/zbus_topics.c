//
// Created by Kirill Shypachov on 15.09.2025.
//
#include "zbus_topics.h"

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <stdbool.h>

//#include "sys/socket.h"
//Subscriber for io_outputs_task
ZBUS_MSG_SUBSCRIBER_DEFINE(io_sub);

//Subscriber for matter_event_loop, with fifo
ZBUS_MSG_SUBSCRIBER_DEFINE(matter_sub);

//Chan for io events
ZBUS_CHAN_DEFINE(io_events,
                io_event_data_t,
                NULL, NULL,
                ZBUS_OBSERVERS(io_sub, matter_sub),
                ZBUS_MSG_INIT(0, 0, 0));


// Канал zbus с состоянием выходов
ZBUS_CHAN_DEFINE(outputs_zbus_topik,            /* имя канала */
                 outputs_msg_t,       /* тип сообщения */
                 NULL, NULL,               /* callback до/после публикации (можно NULL) */
                 ZBUS_OBSERVERS_EMPTY,     /* список наблюдателей */
                 ZBUS_MSG_INIT(0, 0));     /* начальное значение */

// Канал zbus с состоянием входов
ZBUS_CHAN_DEFINE(inputs_zbus_topik,            /* имя канала */
                 outputs_msg_t,       /* тип сообщения */
                 NULL, NULL,               /* callback до/после публикации (можно NULL) */
                 ZBUS_OBSERVERS_EMPTY,     /* список наблюдателей */
                 ZBUS_MSG_INIT(0, 0));     /* начальное значение */

// Канал zbus с состоянием MQTT
ZBUS_CHAN_DEFINE(mqtt_stat_zbus_topik,            /* имя канала */
                 mqtt_status_msg_t,       /* тип сообщения */
                 NULL, NULL,               /* callback до/после публикации (можно NULL) */
                 ZBUS_OBSERVERS_EMPTY,     /* список наблюдателей */
                 ZBUS_MSG_INIT(
                     .seq = 0,
                     .enabled = false,
                     .connected = false));     /* начальное значение */

// Канал zbus с текущими данными енегромониторинга
ZBUS_CHAN_DEFINE(energy_mon_zbus_topik,
                enegry_mon_t,
                NULL, NULL,
                ZBUS_OBSERVERS_EMPTY,
                ZBUS_MSG_INIT(
                    .current = 0,
                    .voltage = 0,
                    .power_factor = 0,
                    .frequency = 0,
                    .energy_kWh_total = 0));