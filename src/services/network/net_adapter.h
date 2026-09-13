/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * network-manager's interface adapter on the board: the W5500 Ethernet and the
 * ESP32-C6 Wi-Fi coprocessor behind the ESP-Hosted driver.
 *
 * The rules of struct network_iface_ops bind it, and three things follow:
 *
 *  - No policy. It starts DHCP when told to configure DHCP and at no other
 *    time (CONFIG_WIFI_STA_AUTO_DHCPV4=n keeps the Wi-Fi driver from doing it
 *    on association), it installs the resolvers it is given, and it joins the
 *    network it is given. Which interface carries traffic, when Wi-Fi is joined
 *    again and whether DNS is manual are network-manager's.
 *  - get_status() and get_dns() never block on the coprocessor. What only the
 *    coprocessor knows — association — is kept from the driver's net_mgmt
 *    events, and RSSI is refreshed by the worker (net_adapter_refresh()).
 *  - Event callbacks copy a flag and wake the worker; they do no work on the
 *    net_mgmt thread every other subscriber shares (plan section 3).
 *
 * Stable ids: the interfaces are found by device — the W5500 by its compatible,
 * Wi-Fi as the station interface — never by net_if index.
 *
 * Not covered in sim: everything here that calls the network stack. The sim
 * tier covers the translations (net_adapter_map.h) and the policy above it
 * (tests/network_manager); the hardware tier covers the rest
 * (docs/device-development/reports/p4).
 */

#ifndef NET_ADAPTER_H_
#define NET_ADAPTER_H_

#include <stdbool.h>

#include <network_manager/network_manager.h>

/** The adapter's operations, for network_manager_init(). */
extern const struct network_iface_ops net_adapter_ops;

/**
 * @brief Find the interfaces and subscribe to their events.
 *
 * @param kick  Wakes the worker; called from the net_mgmt thread.
 * @retval 0        ready; Wi-Fi may still be absent
 * @retval -ENODEV  no Ethernet interface
 */
int net_adapter_init(void (*kick)(void));

/** @brief Refresh what only a blocking call can tell: the Wi-Fi RSSI. Worker only. */
void net_adapter_refresh(void);

/** @brief Whether the Wi-Fi coprocessor completed its initialisation. */
bool net_adapter_wifi_present(void);

#endif /* NET_ADAPTER_H_ */
