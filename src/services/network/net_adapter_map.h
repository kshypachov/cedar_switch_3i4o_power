/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The adapter's translations that need no network stack: Zephyr's Wi-Fi
 * security types to the contract's, and a netmask to a prefix length. Split
 * out so the sim tier checks them; the rest of the adapter needs the board.
 */

#ifndef NET_ADAPTER_MAP_H_
#define NET_ADAPTER_MAP_H_

#include <stdint.h>

#include <zephyr/net/wifi.h>

#include <network_manager/network_manager.h>

/**
 * @brief The contract's security for an access point Zephyr reports as @p type.
 *
 * A type this build cannot join maps to unknown or enterprise, which
 * network-manager makes visible but not selectable. WPA (the first one), WEP,
 * WAPI and DPP are unknown: the contract has no name for them.
 */
enum network_ap_security net_map_wifi_security(enum wifi_security_type type);

/** @brief Leading one bits of a network-order IPv4 netmask. */
uint8_t net_map_prefix_length(const uint8_t mask[4]);

/** @brief The network-order IPv4 netmask of @p prefix_length (0-32). */
void net_map_netmask(uint8_t prefix_length, uint8_t mask[4]);

#endif /* NET_ADAPTER_MAP_H_ */
