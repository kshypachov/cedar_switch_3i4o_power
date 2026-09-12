/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A network interface adapter that can be told what the world looks like.
 *
 * The transaction this module implements is entirely about what happens when
 * the network does *not* come up — a Wi-Fi password that does not work, a
 * cable that is out, DHCP that never answers. None of that can be produced on
 * demand against real hardware, so the sim tier drives it here: each
 * interface's link, address and route are set by the test, and configure()
 * and wifi_connect() can be made to fail.
 *
 * The fake also models the one behaviour that makes the health check
 * meaningful: applying a configuration does not by itself give an interface an
 * address. A DHCP interface gets one only if the test says the server answers,
 * which is how "applied but not working yet" is reached.
 */

#ifndef FAKE_IFACE_H_
#define FAKE_IFACE_H_

#include <stdbool.h>
#include <stdint.h>

#include <network_manager/network_manager.h>

struct fake_iface {
	/* What the test says the physical world is doing. */
	bool present;
	bool link_up;
	/** A DHCP server answers on this segment. */
	bool dhcp_answers;
	/** wifi_connect() succeeds, i.e. the password and SSID are right. */
	bool wifi_associates;
	/**
	 * Withhold the default route while keeping link and address. A gateway
	 * that is configured but does not answer looks exactly like this, and
	 * it is the case that separates a working subnet from a device with no
	 * way off it.
	 */
	bool suppress_route;

	/* What the adapter has been told to do. */
	bool configured;
	bool enabled;
	struct device_config_ipv4 applied;
	unsigned int configure_calls;

	/* Resulting runtime state, derived at get_status() time. */
	bool has_ipv4;
	uint8_t ipv4[4];
	uint8_t prefix_length;
	bool has_route;
};

struct fake_net {
	struct fake_iface eth;
	struct fake_iface wifi;

	/* Wi-Fi association. */
	bool associated;
	uint8_t ssid[DEVICE_CONFIG_SSID_MAX_LEN];
	uint8_t ssid_len;
	uint8_t password[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t password_len;
	unsigned int connect_calls;
	unsigned int disconnect_calls;

	/* DNS. */
	struct device_config_addr dns[DEVICE_CONFIG_DNS_MAX_SERVERS];
	uint8_t dns_count;
	unsigned int set_dns_calls;

	/* Injection. */
	int fail_configure;  /**< errno returned by the next configure(), or 0. */
	int fail_connect;    /**< errno returned by the next wifi_connect(), or 0. */
	int fail_scan;       /**< errno returned by the next wifi_scan(), or 0. */
	bool no_radio;       /**< Present the ops table without wifi_scan. */

	/* Scan. */
	struct network_scan_results scan_results;
	unsigned int scan_calls;
};

/** Reset to a plausible healthy board: Ethernet up with DHCP, Wi-Fi present. */
void fake_net_init(struct fake_net *fn);

/** Fill @p ops with callbacks bound to @p fn. */
void fake_net_bind(struct fake_net *fn, struct network_iface_ops *ops);

#endif /* FAKE_IFACE_H_ */
