//
// Created by Kirill Shypachov on 20.09.2025.
//

#include "ha_mqtt_callback.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include "../zbus_topics.h"
#include <zephyr/zbus/zbus.h>
#include "ha_mqtt_adapter.h"
#include "ha_mqtt_helpers.h"
#include "../msgq_topiks.h"

LOG_MODULE_REGISTER(ha_mqtt_callback);

#define MAX_MQTT_MES_LEN MQTT_PAYLOAD_MAX

void mqtt_evt_handler(struct mqtt_client *const client,
              const struct mqtt_evt *evt)
{
    int err;
    static mqtt_status_msg_t mqtt_conn_stat = {
        0,
        true,
        false
    };

	LOG_DBG("mqtt_evt_handler");

    switch (evt->type) {
        case MQTT_EVT_CONNACK:
            if (evt->result != 0) {
                LOG_ERR("MQTT connect failed %d", evt->result);
                break;
            }

            mqtt_conn_stat.connected = true;
            mqtt_conn_stat.seq = k_uptime_get();
			zbus_chan_pub(&mqtt_stat_zbus_topik, &mqtt_conn_stat, K_NO_WAIT);
            LOG_INF("MQTT client connected!");

#if defined(CONFIG_MQTT_VERSION_5_0)
            if (evt->param.connack.prop.rx.has_topic_alias_maximum &&
                evt->param.connack.prop.topic_alias_maximum > 0) {
                LOG_INF("Topic aliases allowed by the broker, max %u.",
                    evt->param.connack.prop.topic_alias_maximum);

                aliases_enabled = true;
                } else {
                    LOG_INF("Topic aliases disallowed by the broker.");
                }
#endif

#if defined(CONFIG_LOG_BACKEND_MQTT)
            log_backend_mqtt_client_set(client);
#endif

            break;

        case MQTT_EVT_DISCONNECT:
            LOG_INF("MQTT client disconnected %d", evt->result);

            mqtt_conn_stat.connected = false;
            mqtt_conn_stat.seq = k_uptime_get();
			zbus_chan_pub(&mqtt_stat_zbus_topik, &mqtt_conn_stat, K_NO_WAIT);

#if defined(CONFIG_LOG_BACKEND_MQTT)
            log_backend_mqtt_client_set(NULL);
#endif

            break;

        case MQTT_EVT_PUBACK:
            if (evt->result != 0) {
                LOG_ERR("MQTT PUBACK error %d", evt->result);
                break;
            }
            LOG_INF("PUBACK packet id: %u", evt->param.puback.message_id);

            break;

        case MQTT_EVT_PUBLISH:
            LOG_INF("PUBLISH pack recv");
            mqtt_msg_t mqtt_message;
            int rc;

            memset(&mqtt_message, 0, sizeof(mqtt_message));
            mqtt_message.retain = evt->param.publish.retain_flag;
            mqtt_message.qos    = evt->param.publish.message.topic.qos;

            // Топик с гарантией \0
            size_t tn = MIN((size_t)evt->param.publish.message.topic.topic.size,
                            sizeof(mqtt_message.topic) - 1);
            memcpy(mqtt_message.topic,
                   evt->param.publish.message.topic.topic.utf8, tn);
            mqtt_message.topic[tn] = '\0';

            // Полное чтение payload (+drain)
            size_t to_read = evt->param.publish.message.payload.len;
            size_t got = 0;

            if (to_read == 0) {
                mqtt_message.payload[0] = '\0';
            } else {
                while (got < to_read && got < sizeof(mqtt_message.payload) - 1) {
                    int n = mqtt_read_publish_payload(client,
                               mqtt_message.payload + got,
                               sizeof(mqtt_message.payload) - 1 - got);
                    if (n < 0) { LOG_ERR("payload read err %d", n); break; }
                    if (n == 0) { break; }
                    got += n;
                }
                mqtt_message.payload[got] = '\0';

                if (to_read > got) {
                    uint8_t drain_buf[128];
                    size_t remain = to_read - got;
                    while (remain > 0) {
                        int n = mqtt_read_publish_payload(client, drain_buf,
                                      MIN(sizeof(drain_buf), remain));
                        if (n <= 0) { break; }
                        remain -= n;
                    }
                    LOG_WRN("Payload truncated: %zu bytes dropped", to_read - got);
                }
            }

            // Кладём в очередь
            rc = k_msgq_put(&MQTT_MSGQ_RX, &mqtt_message, K_NO_WAIT);
            if (rc == -EAGAIN) {
                LOG_WRN("MQTT_MSGQ_RX full, message dropped");
            } else if (rc) {
                LOG_ERR("MQTT_MSGQ_RX put err %d", rc);
            }

            // ACK-и
            if (mqtt_message.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
                const struct mqtt_puback_param p = { .message_id = evt->param.publish.message_id };
                rc = mqtt_publish_qos1_ack(client, &p);
                if (rc) { LOG_ERR("qos1 ack err %d", rc); }
            } else if (mqtt_message.qos == MQTT_QOS_2_EXACTLY_ONCE) {
                const struct mqtt_pubrec_param p = { .message_id = evt->param.publish.message_id };
                rc = mqtt_publish_qos2_receive(client, &p);
                if (rc) { LOG_ERR("qos2 recv err %d", rc); }
            }

            break;

        // case MQTT_EVT_PUBLISH:
        //     mqtt_msg_t mqtt_message;
        //     int rc = 0;
        //
        //
        //     if (evt->param.publish.message.topic.qos == MQTT_QOS_0_AT_MOST_ONCE)
        //     {
        //         LOG_INF("MQTT resieve message QOS0");
        //         if ( evt->param.publish.message.payload.len > 0 && evt->param.publish.message.payload.len < MAX_MQTT_MES_LEN) {
        //
        //             memset(mqtt_message.payload, 0, sizeof(mqtt_message.payload));
        //             memset(mqtt_message.topic, 0, sizeof(mqtt_message.topic));
        //
        //             mqtt_message.retain = evt->param.publish.retain_flag;
        //             mqtt_message.qos = evt->param.publish.message.topic.qos;
        //             strncpy( mqtt_message.topic, (const char *)evt->param.publish.message.topic.topic.utf8, evt->param.publish.message.topic.topic.size);
        //
        //             size_t to_read = evt->param.publish.message.payload.len;
        //             size_t got = 0;
        //             while (got < to_read && got < sizeof(mqtt_message.payload)) {
        //                 int n = mqtt_read_publish_payload(client, mqtt_message.payload + got,
        //                                                   sizeof(mqtt_message.payload) - got);
        //                 if (n < 0) { LOG_ERR(" QOS0 payload read err %d", n); break; }
        //                 if (n == 0) { break; } // ничего нет прямо сейчас
        //                 got += n;
        //             }
        //
        //             rc = k_msgq_put(&MQTT_MSGQ_RX, &mqtt_message, K_NO_WAIT);
        //             if (rc < 0) {
        //                 LOG_ERR("MQTT PUBLISH QOS0 payload put in the MQTT_MSGQ_RX failed");
        //             }
        //
        //         }else {
        //             LOG_ERR("MQTT PUBLISH QOS0 payload too long");
        //         }
        //     }
        //
        //
        //     if (evt->param.publish.message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE)
        //     {
        //         const struct mqtt_puback_param publish_real_param = {
        //             .message_id = evt->param.publish.message_id,
        //         };
        //         LOG_INF("MQTT resieve message QOS1");
        //         if ( evt->param.publish.message.payload.len > 0 && evt->param.publish.message.payload.len < MAX_MQTT_MES_LEN) {
        //
        //             memset(mqtt_message.payload, 0, sizeof(mqtt_message.payload));
        //             memset(mqtt_message.topic, 0, sizeof(mqtt_message.topic));
        //
        //
        //             mqtt_message.retain = evt->param.publish.retain_flag;
        //             mqtt_message.qos = evt->param.publish.message.topic.qos;
        //             strncpy( mqtt_message.topic, (const char *)evt->param.publish.message.topic.topic.utf8, evt->param.publish.message.topic.topic.size);
        //
        //             size_t to_read = evt->param.publish.message.payload.len;
        //             size_t got = 0;
        //             while (got < to_read && got < sizeof(mqtt_message.payload)) {
        //                 int n = mqtt_read_publish_payload(client, mqtt_message.payload + got,
        //                                                   sizeof(mqtt_message.payload) - got);
        //                 if (n < 0) { LOG_ERR(" QOS1 payload read err %d", n); break; }
        //                 if (n == 0) { break; } // ничего нет прямо сейчас
        //                 got += n;
        //             }
        //
        //             rc = k_msgq_put(&MQTT_MSGQ_RX, &mqtt_message, K_NO_WAIT);
        //             if (rc < 0) {
        //                 LOG_ERR("MQTT PUBLISH QOS1 payload put in the MQTT_MSGQ_RX failed");
        //             }
        //
        //         }else {
        //             LOG_ERR("MQTT PUBLISH QOS1 payload too long");
        //         }
        //
        //         int qos1_ack_rc = mqtt_publish_qos1_ack(client, &publish_real_param);
        //         if (qos1_ack_rc != 0)
        //         {
        //             LOG_ERR("MQTT PUBLISH QOS1 ack failed");
        //         }
        //     }
        //
        //     if (evt->param.publish.message.topic.qos == MQTT_QOS_2_EXACTLY_ONCE)
        //     {
        //         const struct mqtt_pubrec_param pubrec_real_param = {
        //             .message_id = evt->param.publish.message_id,
        //         };
        //
        //         LOG_INF("MQTT resieve message QOS2");
        //         if ( evt->param.publish.message.payload.len > 0 && evt->param.publish.message.payload.len < MAX_MQTT_MES_LEN) {
        //
        //             memset(mqtt_message.payload, 0, sizeof(mqtt_message.payload));
        //             memset(mqtt_message.topic, 0, sizeof(mqtt_message.topic));
        //
        //             mqtt_message.retain = evt->param.publish.retain_flag;
        //             mqtt_message.qos = evt->param.publish.message.topic.qos;
        //             strncpy( mqtt_message.topic, (const char *)evt->param.publish.message.topic.topic.utf8, evt->param.publish.message.topic.topic.size);
        //
        //             size_t to_read = evt->param.publish.message.payload.len;
        //             size_t got = 0;
        //             while (got < to_read && got < sizeof(mqtt_message.payload)) {
        //                 int n = mqtt_read_publish_payload(client, mqtt_message.payload + got,
        //                                                   sizeof(mqtt_message.payload) - got);
        //                 if (n < 0) { LOG_ERR(" QOS2 payload read err %d", n); break; }
        //                 if (n == 0) { break; } // ничего нет прямо сейчас
        //                 got += n;
        //             }
        //
        //             rc = k_msgq_put(&MQTT_MSGQ_RX, &mqtt_message, K_NO_WAIT);
        //
        //             if (rc < 0) {
        //                 LOG_ERR("MQTT PUBLISH QOS2 payload put in the MQTT_MSGQ_RX failed");
        //             }
        //
        //         }else {
        //             LOG_ERR("MQTT PUBLISH QOS2 payload too long");
        //         }
        //
        //
        //         int qos2_ack_rc = mqtt_publish_qos2_receive(client, &pubrec_real_param);
        //         if (qos2_ack_rc != 0)
        //         {
        //             LOG_ERR("MQTT PUBLISH QOS2 ack failed");
        //         }
        //     }
        //
        //
        //
        //     break;

        case MQTT_EVT_PUBREC:
            if (evt->result != 0) {
                LOG_ERR("MQTT PUBREC error %d", evt->result);
                break;
            }

            LOG_INF("PUBREC packet id: %u", evt->param.pubrec.message_id);

            const struct mqtt_pubrel_param rel_param = {
                .message_id = evt->param.pubrec.message_id
            };

            err = mqtt_publish_qos2_release(client, &rel_param);
            if (err != 0) {
                LOG_ERR("Failed to send MQTT PUBREL: %d", err);
            }

            break;

        case MQTT_EVT_PUBREL:
            LOG_INF("PUBREL packet id: %u", evt->param.pubrel.message_id);
            const struct mqtt_pubcomp_param comp = { .message_id = evt->param.pubrel.message_id };
            err = mqtt_publish_qos2_complete(client, &comp);
            if (err) { LOG_ERR("Failed to send PUBCOMP: %d", err); }
            break;

        case MQTT_EVT_PUBCOMP:
            if (evt->result != 0) {
                LOG_ERR("MQTT PUBCOMP error %d", evt->result);
                break;
            }

            LOG_INF("PUBCOMP packet id: %u",
                evt->param.pubcomp.message_id);

            break;

        case MQTT_EVT_PINGRESP:
            LOG_INF("PINGRESP packet");
            break;


        default:
            break;
    }
}


