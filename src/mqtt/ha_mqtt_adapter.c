// //
// // Created by Kirill Shypachov on 22.09.2025.
// //
//
// #include "ha_mqtt_adapter.h"
// #include "../zbus_topics.h"
// #include <zephyr/net/socket.h>
// #include <zephyr/net/socket_service.h>
// #include <zephyr/settings/settings.h>
// #include <zephyr/net/sntp.h>
// #include <zephyr/kernel.h>
// #include <zephyr/zbus/zbus.h>
// #include <zephyr/logging/log.h>
// #include <arpa/inet.h>
// #include <zephyr/net/mqtt.h>
// #include <zephyr/net/net_if.h>
// #include <zephyr/net/net_ip.h>      // net_addr_ntop, NET_IPVx
// #include <zephyr/drivers/hwinfo.h>
// #include <errno.h>
//
// #include <zephyr/random/random.h>
// #include "../settings_topics.h"
// #include "../global_var.h"
// #include "ha_mqtt_helpers.h"
// #include "../msgq_topiks.h"
//
// LOG_MODULE_REGISTER(ha_mqtt_adapter);
//
// #define mqtt_qos_for_subscrabe MQTT_QOS_2_EXACTLY_ONCE
//
// static uint16_t next_mid = 1;  // 1..65535, 0 запрещён
//
// static inline uint16_t alloc_mid(void) {
//     if (++next_mid == 0) { next_mid = 1; }
//     return next_mid;
// }
//
//
// static int ha_mqtt_set_unic_id(void) {
//     uint8_t id[32] = {0};                    // размер с запасом (реально 8..32 байт)
//     ssize_t n = hwinfo_get_device_id(id, sizeof(id));
//     if (n < 0) {
//         LOG_ERR("hwinfo_get_device_id rc=%d", (int)n); // -ENOTSUP если не поддерживается
//         return (int)n;
//     }
//
//     set_device_id(id, (unsigned int)n);
//     return 0;
// }
//
// static int get_default_iface_ip_str(char *out_buf, size_t out_len)
// {
//     if (!out_buf || out_len == 0) {
//         return -EINVAL;
//     }
//
//     struct net_if *iface = net_if_get_default();
//     if (!iface) {
//         LOG_WRN("No default net_if");
//         return -ENODEV;
//     }
//
// #if defined(CONFIG_NET_IPV4)
//     /* пытаемся взять глобальный IPv4 со статусом preferred */
//     const struct in_addr *v4 = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
//     if (v4) {
//         if (!net_addr_ntop(AF_INET, v4, out_buf, out_len)) {
//             return -EINVAL;
//         }
//         return 0;
//     }
// #endif
//
// #if defined(CONFIG_NET_IPV6)
//     /* если IPv4 нет — пробуем глобальный IPv6 (preferred) */
//     //const struct in6_addr *v6 = net_if_ipv6_get_global_addr(NET_ADDR_PREFERRED, iface );
//     if (v6) {
//         if (!net_addr_ntop(AF_INET6, v6, out_buf, out_len)) {
//             return -EINVAL;
//         }
//         return 0;
//     }
// #endif
//
//     /* Адрес ещё не получен (например, DHCPv4 не завершился) */
//     return -EAGAIN;
// }
//
// static int ha_mqtt_set_dev_ip(void) {
//
//     char ip_str[INET6_ADDRSTRLEN] = {0};
//     int rc = get_default_iface_ip_str(ip_str, sizeof(ip_str));
//     if (rc < 0) {
//         LOG_ERR("get_default_iface_ip_str failed (%d)", rc);
//         return rc;
//     }
//
//     rc = set_device_conf_ip(ip_str);
//     if (rc < 0) {
//         LOG_ERR("set_device_conf_ip(%s) failed (rc=%d)", ip_str, rc);
//         return rc;
//     }
//
//     LOG_INF("Device IP set to %s", ip_str);
//     return 0;
// }
//
// int ha_mqtt_init(void) {
//
//     int rc = ha_mqtt_set_unic_id();
//     if (rc < 0) {
//         LOG_ERR("ha_mqtt_set_unic_id failed (%d)", rc);
//         return rc;
//     }
//     rc = ha_mqtt_set_dev_ip();
//     if (rc < 0) {
//         LOG_ERR("ha_mqtt_set_dev_ip failed (%d)", rc);
//         return rc;
//     }
//     return 0;
// }
//
// int ha_mqtt_subscribe_topiks(const struct mqtt_client *client) {
//
//     char topics_str[device_relay_count][mqtt_topik_max_len] = {0};
//     struct mqtt_subscription_list ha_mqtt_subscribe_topiks_list;
//     struct mqtt_topic ha_subs_topics[device_relay_count];
//
//
//     for (int i = 0; i < device_relay_count; i++) {
//         int rc = generate_command_topik_for_subscrabe(topics_str[i], mqtt_topik_max_len, OUTPUT_DEVICE, i+1);
//         if (rc < 0) {
//             LOG_ERR("generate_command_topik_for_subscrabe failed (%d)", rc);
//             return rc;
//         }
//         ha_subs_topics[i].topic.utf8 = (uint8_t *) topics_str[i];
//         ha_subs_topics[i].topic.size = strlen(topics_str[i]);
//         ha_subs_topics[i].qos = mqtt_qos_for_subscrabe;
//     }
//
//     ha_mqtt_subscribe_topiks_list.list = ha_subs_topics;
//     ha_mqtt_subscribe_topiks_list.list_count = device_relay_count;
//     ha_mqtt_subscribe_topiks_list.message_id = (uint16_t)(sys_rand16_get() ?: 1);
//
//     int subs_rc = mqtt_subscribe(client, &ha_mqtt_subscribe_topiks_list);
//
//     if (subs_rc < 0) {
//         LOG_ERR("mqtt_subscribe failed (%d)", subs_rc);
//     } else {
//         LOG_INF("mqtt_subscribe success (%d)", subs_rc);
//     }
//
//     return subs_rc;
// }
//
// const char *ha_mqtt_get_device_id(void) {
//     return get_device_id();
// }
//
// // int ha_send_data_from_q_to_mqtt( struct mqtt_client *client, struct mqtt_publish_param *pub_param) {
// //
// //     mqtt_msg_t peek_msg;
// //
// //     if (client == NULL || pub_param == NULL) {
// //         return -EINVAL;
// //     }
// //
// //     if (k_msgq_peek(&MQTT_MSGQ_TX, &peek_msg) == 0) {
// //         /* В peek_msg — копия первого сообщения.
// //            Элемент остаётся в очереди, его потом можно будет
// //            забрать обычным k_msgq_get(). */
// //         pub_param->message_id = sys_rand16_get();
// //         pub_param->dup_flag = false;
// //         pub_param->message.topic.topic.utf8 = (uint8_t *)peek_msg.topic;
// //         pub_param->message.topic.topic.size = strlen(peek_msg.topic);
// //         pub_param->message.payload.data = (uint8_t *)peek_msg.payload;
// //         pub_param->message.payload.len = strlen(peek_msg.payload);
// //         pub_param->message.topic.qos = peek_msg.qos;
// //         pub_param->retain_flag = peek_msg.retain;
// //
// //         int pub_rc = mqtt_publish(client, pub_param);
// //         if (pub_rc != 0) {
// //             LOG_ERR("Can't send HA MQTT publish message mqtt_publish() = %d", pub_rc);
// //             return pub_rc;
// //         }else {
// //             LOG_INF("HA MQTT publish message sent");
// //             k_msgq_get(&MQTT_MSGQ_TX, &peek_msg, K_NO_WAIT);
// //             return pub_rc;
// //         }
// //
// //     } else {
// //         LOG_INF("No data for sending in MQTT_MSGQ_TX queue");
// //     }
// //     return 0;
// //
// // }
//
// int ha_send_data_from_q_to_mqtt(struct mqtt_client *client,
//                                 struct mqtt_publish_param *pub_param)
// {
//     mqtt_msg_t peek_msg;
//
//     if (!client || !pub_param) {
//         return -EINVAL;
//     }
//
//     /* Если нет данных — возвращаем -ENODATA и НЕ логируем "publish success". */
//     if (k_msgq_peek(&MQTT_MSGQ_TX, &peek_msg) != 0) {
//         /* опционально: LOG_DBG, чтобы не засорять логи */
//         // LOG_DBG("MQTT_TX empty");
//         return -ENODATA;
//     }
//
//     /* Заполняем параметры публикации аккуратно */
//     memset(pub_param, 0, sizeof(*pub_param));
//
//     pub_param->retain_flag = peek_msg.retain;
//     pub_param->dup_flag    = false;
//
//     /* QoS */
//     pub_param->message.topic.qos = peek_msg.qos;
//
//     /* Topic */
//     size_t tlen = strnlen(peek_msg.topic, sizeof(peek_msg.topic));
//     pub_param->message.topic.topic.utf8 = (uint8_t *)peek_msg.topic;
//     pub_param->message.topic.topic.size = tlen;
//
//     /* Payload (если у вас бинарные данные, храните явную длину, а не strlen) */
//     size_t plen = strnlen(peek_msg.payload, sizeof(peek_msg.payload));
//     pub_param->message.payload.data = (uint8_t *)peek_msg.payload;
//     pub_param->message.payload.len  = plen;
//
//     /* MID только для QoS1/2. Для QoS0 можно оставить 0. */
//     if (peek_msg.qos == MQTT_QOS_1_AT_LEAST_ONCE ||
//         peek_msg.qos == MQTT_QOS_2_EXACTLY_ONCE) {
//         pub_param->message_id = alloc_mid();  // монотонный, !=0
//     } else {
//         pub_param->message_id = 0;
//     }
//
//     if (tlen == 0) {
//         LOG_ERR("MQTT TX msg dropped: empty topic. Payload: %s", peek_msg.payload);
//         // снимем плохое сообщение и дальше работаем
//         (void)k_msgq_get(&MQTT_MSGQ_TX, &peek_msg, K_NO_WAIT);
//         return -EINVAL;
//     }
//
//
//     int rc = mqtt_publish(client, pub_param);
//     if (rc != 0) {
//         LOG_ERR("mqtt_publish() = %d", rc);
//         return rc;  /* элемент НЕ снимаем с очереди — попробуем позже */
//     }
//
//     /* ВАЖНО: снимать элемент с очереди только после успешного mqtt_publish().
//        (Для идеала — после PUBACK/PUBCOMP. См. блок ниже.) */
//     (void)k_msgq_get(&MQTT_MSGQ_TX, &peek_msg, K_NO_WAIT);
//     LOG_INF("HA MQTT publish message sent (mid=%u, qos=%d, tlen=%u, plen=%u)",
//             (unsigned)pub_param->message_id, (int)peek_msg.qos,
//             (unsigned)tlen, (unsigned)plen);
//     return 0;
// }
//
// int ha_mqtt_send_inputs_state(struct mqtt_client *client) {
//     return 0;
// }