// //
// // Created by Kirill Shypachov on 14.09.2025.
// //
// #include "ha_mqtt.h"
// #include "../zbus_topics.h"
// #include <zephyr/logging/log.h>
// #include <zephyr/net/socket.h>
// #include <zephyr/net/socket_service.h>
// #include <zephyr/settings/settings.h>
// #include <zephyr/net/sntp.h>
// #include <zephyr/kernel.h>
// #include <zephyr/zbus/zbus.h>
// #include <arpa/inet.h>
// #include <zephyr/net/mqtt.h>
// #include <zephyr/random/random.h>
//
// #include "ha_mqtt_adapter.h"
// #include "../settings_topics.h"
//
// #include "ha_mqtt_callback.h"
// #include "ha_task.h"
//
// #define MQTT_TASK_PRIORITY 4
//
// LOG_MODULE_REGISTER(mqtt_ha);
//
// K_THREAD_STACK_DEFINE(mqtt_task_stack, 8192);
// static struct k_thread mqtt_task_thread_data;
//
// mqtt_settings_t mqtt_sett = {0};
// struct mqtt_utf8 username;
// struct mqtt_utf8 password;
//
// /* The mqtt client struct */
// static struct mqtt_client client_ctx;
//
// #define APP_MQTT_BUFFER_SIZE	2048
//
// /* Buffers for MQTT client. */
// static uint8_t rx_buffer[APP_MQTT_BUFFER_SIZE];
// static uint8_t tx_buffer[APP_MQTT_BUFFER_SIZE];
//
// #define SEC_TAG_SERVER_PIN  42
//
// static int dns_query(const char *host, uint16_t port, int family, int socktype, struct sockaddr *addr,
//               socklen_t *addrlen)
// {
//     struct addrinfo hints = {
//         .ai_family = family,
//         .ai_socktype = socktype,
//     };
//     struct addrinfo *res = NULL;
//     char addr_str[INET6_ADDRSTRLEN] = {0};
//     int rv;
//
//     /* Perform DNS query */
//     rv = getaddrinfo(host, NULL, &hints, &res);
//     if (rv < 0) {
//         LOG_ERR("getaddrinfo failed (%d, errno %d)", rv, errno);
//         return rv;
//     }
//     /* Store the first result */
//     *addr = *res->ai_addr;
//     *addrlen = res->ai_addrlen;
//     /* Free the allocated memory */
//     freeaddrinfo(res);
//     /* Store the port */
//     net_sin(addr)->sin_port = htons(port);
//     /* Print the found address */
//     inet_ntop(addr->sa_family, &net_sin(addr)->sin_addr, addr_str, sizeof(addr_str));
//     LOG_INF("%s -> %s", host, addr_str);
//     return 0;
// }
//
// static void client_init(struct mqtt_client *client, struct sockaddr *addr)
// {
// 	mqtt_client_init(client);
//
// 	//broker_init();
// 	//broker = addr;
// 	/* MQTT client configuration */
// 	//client->broker = &broker;
// 	struct mqtt_sec_config *tls_config = &client->transport.tls.config;
//
// 	username.utf8 = mqtt_sett.user;
// 	username.size = strlen(mqtt_sett.user);
//
// 	password.utf8 = mqtt_sett.pass;
// 	password.size = strlen(mqtt_sett.pass);
//
// 	client->broker = addr;
// 	client->evt_cb = mqtt_evt_handler;
// 	client->client_id.utf8 = (uint8_t *)ha_mqtt_get_device_id();
// 	client->client_id.size = strlen(ha_mqtt_get_device_id());
// 	client->password = &password;
// 	client->user_name = &username;
// 	client->protocol_version = MQTT_VERSION_3_1_1;
//
// 	/* MQTT buffers configuration */
// 	client->rx_buf = rx_buffer;
// 	client->rx_buf_size = sizeof(rx_buffer);
// 	client->tx_buf = tx_buffer;
// 	client->tx_buf_size = sizeof(tx_buffer);
//
// 	/* MQTT transport configuration */
// 	client->transport.type = MQTT_TRANSPORT_SECURE;
// 	client->transport.tls.sock =
//
// 	tls_config->peer_verify = TLS_PEER_VERIFY_NONE;
// 	tls_config->cipher_list = NULL;
// 	tls_config->sec_tag_list = NULL;
// 	tls_config->sec_tag_count = 0;
// 	tls_config->hostname = NULL;
// //	client->transport.tls.sock = zsock_socket(addr->sa_family, SOCK_STREAM, IPPROTO_TLS_1_2);
//
// 	client->keepalive = 30;
// 	client->clean_session = 1;
// }
//
// static void mqtt_task(void *a, void *b, void *c) {
//
//     ARG_UNUSED(a);
//     ARG_UNUSED(b);
//     ARG_UNUSED(c);
//
//     int family = AF_INET;
//     char *family_str = family == AF_INET ? "IPv4" : "IPv6";
//     struct sockaddr addr;
//     socklen_t addrlen;
//     int rv;
// 	int mqtt_subscrabed = 0;
//
//     #define PUB_PERIOD_MS 100
//
//     static uint64_t next_pub_ms;
//
//
//     while (1) {
//
//         rv = dns_query(mqtt_sett.host, mqtt_sett.port, family, SOCK_DGRAM, &addr, &addrlen);
//         if (rv != 0) {
//             LOG_ERR("Failed to lookup %s MQTT server ", family_str);
//             LOG_ERR("DNS query failed (%d)", rv);
//             LOG_ERR("Waiting 10s and retry");
//         }else {
//         	client_init(&client_ctx, &addr);
//             LOG_INF("MQTT server: %s port: %d", inet_ntoa(net_sin(&addr)->sin_addr), mqtt_sett.port);
//         	int rc = mqtt_connect(&client_ctx);
//
//         	if (rc != 0) {
//         		LOG_ERR("MQTT conection fail, err: %d", rc);
//         	    LOG_ERR("Wait 10s and retry");
//         		k_sleep(K_SECONDS(10));
//         		continue;
//         	}
//             LOG_INF("MQTT TCP sesion connected ");
//
//             mqtt_status_msg_t mqtt_conn_status = {
//                 0,
//                 true,
//                 false
//             };
//             zbus_chan_pub(&mqtt_stat_zbus_topik, &mqtt_conn_status, K_NO_WAIT);
//
//         	for (int i = 0; i < 100; i++) {
//
// 				LOG_INF("Waiting initial connection. Iteration : %d", i);
//         		rc = mqtt_input(&client_ctx);
//         		if (rc != 0 && rc != -EAGAIN) {
//         			LOG_ERR("mqtt_input rc=%d", rc);
//         			break;
//         		}
//
//         		rc = mqtt_live(&client_ctx);
//         		if (rc != 0 && rc != -EAGAIN) {
//         			LOG_ERR("mqtt_live rc=%d", rc);
//         			break;
//         		}
//         		k_sleep(K_SECONDS(1));
//
//         		zbus_chan_read(&mqtt_stat_zbus_topik, &mqtt_conn_status,K_NO_WAIT);
//         		if (mqtt_conn_status.connected) break;
//         	}
//
//
//         	mqtt_subscrabed = 0;
//
//         	while (1) {
//
//         		rc = mqtt_input(&client_ctx);
//         		if (rc != 0 && rc != -EAGAIN) {
//         			LOG_ERR("mqtt_input rc=%d", rc);
//         			break;
//         		}
//
//         		rc = mqtt_live(&client_ctx);
//         		if (rc != 0 && rc != -EAGAIN) {
//         			LOG_ERR("mqtt_live rc=%d", rc);
//         			break;
//         		}
//         	    zbus_chan_read(&mqtt_stat_zbus_topik, &mqtt_conn_status,K_NO_WAIT);
//         	    if (!mqtt_conn_status.connected)
//         	    {
//         	        LOG_INF("MQTT disconnected. Read from zbus chan");
//         	        break;
//         	    }
//
//         		if (mqtt_subscrabed) {
//
//         			struct mqtt_publish_param pub;
//         			int pub_rc = ha_send_data_from_q_to_mqtt(&client_ctx, &pub);
//         			//LOG_INF("MQTT publish rc=%d", pub_rc);
//
//         		}else {
//
//         			if (ha_mqtt_init() < 0) {
//         				LOG_ERR("ha_mqtt_init failed");
//         				mqtt_disconnect(&client_ctx, NULL);
//         			}
//
//         			if (ha_mqtt_subscribe_topiks(&client_ctx) < 0) {
//         				LOG_ERR("ha_mqtt_subscribe_topiks failed");
//         				mqtt_disconnect(&client_ctx, NULL);
//         			}else {
//         				LOG_INF("MQTT subscribe success!");
//         				mqtt_subscrabed = 1;
//         			}
//         		}
//
//         		k_sleep(K_MSEC(100)); /* 10 Гц: достаточно, чтобы не пропускать keepalive и события */
//         	}
//
//         }
//         k_sleep(K_SECONDS(10));
//     }
// 	return;
// }
//
// void app_mqtt_ha_client_init(void) {
//
// 	settings_load_one(mqtt_port_settings, &mqtt_sett.port, sizeof(mqtt_sett.port));
// 	settings_load_one(mqtt_enabled_settings, &mqtt_sett.enabled, sizeof(mqtt_sett.enabled));
// 	settings_load_one(mqtt_host_settings, mqtt_sett.host, sizeof(mqtt_sett.host));
// 	settings_load_one(mqtt_user_settings, mqtt_sett.user, sizeof(mqtt_sett.user));
// 	settings_load_one(mqtt_pass_settings, mqtt_sett.pass, sizeof(mqtt_sett.pass));
//
// 	if (mqtt_sett.enabled) {
//
// 		k_tid_t tid = k_thread_create(&mqtt_task_thread_data,
// 		                          mqtt_task_stack,
// 		                          K_THREAD_STACK_SIZEOF(mqtt_task_stack),
// 		                          mqtt_task,
// 		                          NULL, NULL, NULL,
// 		                          MQTT_TASK_PRIORITY,
// 		                          0,
// 		                          K_NO_WAIT);
//
// 		k_thread_name_set(tid, "mqtt_task_ha");
//
// 		start_ha_mqtt_task();
// 	}
// }