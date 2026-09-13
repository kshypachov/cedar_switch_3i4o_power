/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The network on the board: network-manager with its store in
 * settings-registry, the interface adapter, the physical recovery rule, and the
 * one worker thread that does all interface work.
 *
 * Plan section 3 puts adapters in src/services/. This is the only place that
 * knows which pieces make the board's network; main.c starts it and connects
 * API v1 to network_service_kick().
 */

#ifndef NETWORK_SERVICE_H_
#define NETWORK_SERVICE_H_

#include <stdint.h>

/**
 * @brief Start the network.
 *
 * Loads the stored configuration (rolling back a change a reboot interrupted),
 * counts this boot towards the five-power-cycle recovery, binds the adapter,
 * and starts the worker, whose first pass puts the committed configuration on
 * the interfaces — on a device that never had one, Ethernet on DHCP.
 *
 * Needs setting_registry_init() and job_manager_init() before it, and the
 * Ethernet MAC set (ethernet_interfaces_init()).
 *
 * @retval 0  started; errors of the store or the adapter are logged and the
 *            service runs on the factory configuration rather than not at all
 */
int network_service_start(void);

/** @brief Wake the worker: a request was accepted and has interface work waiting. */
void network_service_kick(void);

/** @brief Wi-Fi security modes this build joins, as BIT(enum device_config_wifi_security). */
uint32_t network_service_wifi_security_modes(void);

#endif /* NETWORK_SERVICE_H_ */
