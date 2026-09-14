/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A network interface adapter that can be told what the world looks like.
 *
 * The transaction network-manager implements is entirely about what happens
 * when the network does *not* come up — a Wi-Fi password that does not work, a
 * cable that is out, DHCP that never answers. None of that can be produced on
 * demand against real hardware, so the sim tier drives it here: each
 * interface's link, address and route are set by the test, and configure()
 * and wifi_connect() can be made to fail.
 *
 * The fake also models the one behaviour that makes the health check
 * meaningful: applying a configuration does not by itself give an interface an
 * address. A DHCP interface gets one only if the test says the server answers,
 * which is how "applied but not working yet" is reached.
 *
 * Shared by the network-manager suite and the web API suite, which drives the
 * v1 network bindings over the same fake.
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
	uint8_t mac[6];
	/** Reported after the IPv4 address, e.g. a link-local IPv6 one. */
	struct network_addr extra[2];
	uint8_t extra_count;

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
	/** An association that has been asked for and has no answer yet. */
	bool connecting;
	bool connect_failed;
	/** wifi_connect() leaves the attempt pending instead of settling it. */
	bool connect_pending;
	uint8_t ssid[DEVICE_CONFIG_SSID_MAX_LEN];
	uint8_t ssid_len;
	/** Reported while associated; -55 after fake_net_init(). */
	int8_t rssi;
	uint8_t password[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t password_len;
	unsigned int connect_calls;
	unsigned int disconnect_calls;

	/* DNS: what was installed, and what the resolver reports. */
	struct device_config_addr dns[DEVICE_CONFIG_DNS_MAX_SERVERS];
	uint8_t dns_count;
	unsigned int set_dns_calls;
	/** get_dns() reports @ref dns_seen, as if DHCP had replaced the list. */
	bool dns_override;
	struct device_config_addr dns_seen[DEVICE_CONFIG_DNS_MAX_SERVERS];
	uint8_t dns_seen_count;

	/* Default route. */
	bool has_default;
	enum device_config_interface default_iface;
	unsigned int set_default_calls;

	/* Injection. */
	int fail_configure;  /**< errno returned by the next configure(), or 0. */
	int fail_connect;    /**< errno returned by the next wifi_connect(), or 0. */
	int fail_scan;       /**< errno returned by the next wifi_scan(), or 0. */
	bool no_radio;       /**< Present the ops table without wifi_scan. */
	bool no_get_dns;     /**< Present the ops table without get_dns. */
	bool no_set_default; /**< Present the ops table without set_default. */
	/**
	 * Run once inside the next adapter call that changes something, while
	 * network-manager has released its mutex: a request arriving mid-push.
	 */
	void (*during_io)(void *arg);
	void *during_io_arg;

	/* The coprocessor's other owners (coprocessor-manager on the board). */
	/** Present exclusive_claim and exclusive_release in the ops table. */
	bool exclusive_hooks;
	/** errno a claim of that kind is refused with, or 0 to grant it. Sticky. */
	int refuse_claim[NETWORK_EXCLUSIVE_COUNT];
	unsigned int refused[NETWORK_EXCLUSIVE_COUNT];
	unsigned int claims[NETWORK_EXCLUSIVE_COUNT];
	unsigned int releases[NETWORK_EXCLUSIVE_COUNT];
	/** Releases of a claim that was not held: a bug in network-manager. */
	unsigned int unbalanced_releases;

	/* Scan. */
	struct network_scan_results scan_results;
	unsigned int scan_calls;
};

/** Reset to a plausible healthy board: Ethernet up with DHCP, Wi-Fi present. */
void fake_net_init(struct fake_net *fn);

/** Fill @p ops with callbacks bound to @p fn. */
void fake_net_bind(struct fake_net *fn, struct network_iface_ops *ops);

#endif /* FAKE_IFACE_H_ */
