/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See net_adapter.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
/* net_if.h first: dhcpv4.h names struct net_if without declaring it. */
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "net_adapter.h"
#include "net_adapter_map.h"

LOG_MODULE_REGISTER(net_adapter, LOG_LEVEL_INF);

/* RSSI changes slowly and asking costs an RPC to the coprocessor. */
#define RSSI_REFRESH_MS 10000

/* What configure() last did to an interface, so an unchanged interface is not
 * touched again: restarting DHCP on it would take away the address a browser
 * is using. */
struct applied {
	bool known;
	bool enabled;
	struct device_config_ipv4 ipv4;
};

static struct {
	struct net_if *eth;
	struct net_if *wifi;
	void (*kick)(void);
	struct applied applied[DEVICE_CONFIG_INTERFACE_COUNT];

	/* Wi-Fi association, from the driver's events. */
	atomic_t connecting;
	atomic_t associated;
	atomic_t failed;

	struct k_spinlock lock; /* the members below */
	uint8_t ssid[DEVICE_CONFIG_SSID_MAX_LEN];
	uint8_t ssid_len;
	int8_t rssi;
	bool rssi_valid;
	int64_t rssi_at_ms;

	/* The manual resolvers installed, to take out again. */
	struct device_config_addr manual_dns[DEVICE_CONFIG_DNS_MAX_SERVERS];
	uint8_t manual_dns_count;
	/* get_dns() when the resolver is busy: the last answer. */
	struct device_config_addr last_dns[DEVICE_CONFIG_DNS_MAX_SERVERS];
	uint8_t last_dns_count;

	/* The scan being filled; worker only. */
	struct network_scan_results *scan_out;
} ad;

static struct net_mgmt_event_callback iface_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct net_mgmt_event_callback ipv6_cb;
static struct net_mgmt_event_callback wifi_cb;

static struct net_if *pick(enum device_config_interface iface)
{
	return (iface == DEVICE_CONFIG_INTERFACE_ETHERNET) ? ad.eth : ad.wifi;
}

static bool wifi_ready(void)
{
	return ad.wifi != NULL && device_is_ready(net_if_get_device(ad.wifi));
}

bool net_adapter_wifi_present(void)
{
	return wifi_ready();
}

/* -- events: copy, wake, return ------------------------------------------------- */

static void on_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = cb->info;
		const bool joined = status != NULL && status->status == 0;

		atomic_set(&ad.connecting, 0);
		atomic_set(&ad.associated, joined);
		atomic_set(&ad.failed, !joined);
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		atomic_set(&ad.connecting, 0);
		atomic_set(&ad.associated, 0);
	}

	if (ad.kick != NULL) {
		ad.kick();
	}
}

/* -- status ---------------------------------------------------------------------- */

struct addr_walk {
	struct network_iface_status *out;
};

static void ipv4_address(struct net_if *iface, struct net_if_addr *addr, void *user_data)
{
	struct addr_walk *walk = user_data;
	struct network_iface_status *out = walk->out;
	struct net_in_addr mask;
	struct network_addr *a;

	if (out->addr_count >= NETWORK_IFACE_MAX_ADDRS) {
		return;
	}
	a = &out->addrs[out->addr_count++];
	a->family = DEVICE_CONFIG_AF_INET;
	memcpy(a->bytes, addr->address.in_addr.s4_addr, 4);
	mask = net_if_ipv4_get_netmask_by_addr(iface, &addr->address.in_addr);
	a->prefix_length = net_map_prefix_length(mask.s4_addr);
	/* The DHCP client adds its lease as NET_ADDR_DHCP; configure() adds MANUAL. */
	a->source = (addr->addr_type == NET_ADDR_DHCP) ? NETWORK_ADDR_DHCP : NETWORK_ADDR_STATIC;

	if (!out->has_ipv4) {
		out->has_ipv4 = true;
		memcpy(out->ipv4, a->bytes, 4);
		out->prefix_length = a->prefix_length;
	}
}

static void ipv6_address(struct net_if *iface, struct net_if_addr *addr, void *user_data)
{
	struct addr_walk *walk = user_data;
	struct network_iface_status *out = walk->out;
	struct network_addr *a;

	ARG_UNUSED(iface);

	if (out->addr_count >= NETWORK_IFACE_MAX_ADDRS) {
		return;
	}
	a = &out->addrs[out->addr_count++];
	a->family = DEVICE_CONFIG_AF_INET6;
	memcpy(a->bytes, addr->address.in6_addr.s6_addr, 16);
	a->prefix_length = 64;
	if (net_ipv6_is_ll_addr(&addr->address.in6_addr)) {
		a->source = NETWORK_ADDR_LINK_LOCAL;
	} else if (addr->addr_type == NET_ADDR_AUTOCONF) {
		a->source = NETWORK_ADDR_SLAAC;
	} else if (addr->addr_type == NET_ADDR_DHCP) {
		a->source = NETWORK_ADDR_DHCP;
	} else {
		a->source = NETWORK_ADDR_STATIC;
	}
}

static int get_status(void *ctx, enum device_config_interface iface,
		      struct network_iface_status *out)
{
	struct net_if *ni = pick(iface);
	struct addr_walk walk = {.out = out};

	ARG_UNUSED(ctx);
	memset(out, 0, sizeof(*out));

	if (ni == NULL) {
		return 0;
	}
	out->present = (iface == DEVICE_CONFIG_INTERFACE_WIFI)
			       ? wifi_ready()
			       : device_is_ready(net_if_get_device(ni));

	const struct net_linkaddr *link = net_if_get_link_addr(ni);

	if (link != NULL && link->len == sizeof(out->mac)) {
		memcpy(out->mac, link->addr, sizeof(out->mac));
	}
	if (!out->present) {
		return 0;
	}

	/* Operationally up: carrier for Ethernet, associated (not dormant) for Wi-Fi. */
	out->link_up = net_if_oper_state(ni) == NET_IF_OPER_UP;

	net_if_ipv4_addr_foreach(ni, ipv4_address, &walk);
	net_if_ipv6_addr_foreach(ni, ipv6_address, &walk);

	const struct net_in_addr gw = net_if_ipv4_get_gw(ni);

	out->has_gateway = gw.s_addr != 0U;
	memcpy(out->gateway, gw.s4_addr, 4);
	out->has_route = out->link_up && out->has_ipv4 && out->has_gateway;

	if (iface == DEVICE_CONFIG_INTERFACE_WIFI) {
		k_spinlock_key_t key = k_spin_lock(&ad.lock);

		out->wifi_associated = atomic_get(&ad.associated) != 0;
		out->wifi_connecting = atomic_get(&ad.connecting) != 0;
		out->wifi_failed = atomic_get(&ad.failed) != 0;
		out->ssid_len = ad.ssid_len;
		memcpy(out->ssid, ad.ssid, ad.ssid_len);
		out->rssi = ad.rssi;
		out->rssi_valid = ad.rssi_valid && out->wifi_associated;
		k_spin_unlock(&ad.lock, key);
	}

	return 0;
}

void net_adapter_refresh(void)
{
	struct wifi_iface_status status = {0};
	const int64_t now = k_uptime_get();

	if (!wifi_ready() || atomic_get(&ad.associated) == 0 ||
	    now - ad.rssi_at_ms < RSSI_REFRESH_MS) {
		return;
	}
	ad.rssi_at_ms = now;
	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, ad.wifi, &status, sizeof(status)) != 0) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&ad.lock);

	ad.rssi = (int8_t)CLAMP(status.rssi, INT8_MIN, 0);
	ad.rssi_valid = status.state >= WIFI_STATE_ASSOCIATED;
	k_spin_unlock(&ad.lock, key);
}

/* -- addressing ------------------------------------------------------------------ */

struct ipv4_list {
	struct net_in_addr addrs[CONFIG_NET_IF_MAX_IPV4_COUNT];
	size_t count;
};

static void collect_ipv4(struct net_if *iface, struct net_if_addr *addr, void *user_data)
{
	struct ipv4_list *list = user_data;

	ARG_UNUSED(iface);
	if (list->count < ARRAY_SIZE(list->addrs)) {
		list->addrs[list->count++] = addr->address.in_addr;
	}
}

/* Every IPv4 address and the gateway off the interface; IPv6 is left alone. */
static void clear_ipv4(struct net_if *ni)
{
	struct ipv4_list list = {0};

	net_dhcpv4_stop(ni);
	net_if_ipv4_addr_foreach(ni, collect_ipv4, &list);
	for (size_t i = 0; i < list.count; i++) {
		(void)net_if_ipv4_addr_rm(ni, &list.addrs[i]);
	}
	net_if_ipv4_set_gw(ni, &(struct net_in_addr){0});
}

static int configure(void *ctx, enum device_config_interface iface,
		     const struct device_config_ipv4 *ipv4, bool enabled)
{
	struct net_if *ni = pick(iface);
	struct applied *applied = &ad.applied[iface];

	ARG_UNUSED(ctx);

	if (ni == NULL) {
		return enabled ? -ENODEV : 0;
	}
	if (iface == DEVICE_CONFIG_INTERFACE_WIFI && enabled && !wifi_ready()) {
		return -ENODEV;
	}
	if (applied->known && applied->enabled == enabled &&
	    memcmp(&applied->ipv4, ipv4, sizeof(*ipv4)) == 0) {
		return 0;
	}

	clear_ipv4(ni);
	*applied = (struct applied){.known = true, .enabled = enabled, .ipv4 = *ipv4};

	if (!enabled) {
		/* Wi-Fi stays up: its driver keeps the station interface administratively up. */
		if (iface == DEVICE_CONFIG_INTERFACE_ETHERNET) {
			(void)net_if_down(ni);
		}
		LOG_INF("%s disabled", device_config_interface_str(iface));
		return 0;
	}
	if (!net_if_is_admin_up(ni)) {
		(void)net_if_up(ni);
	}

	if (ipv4->mode == DEVICE_CONFIG_IPV4_DHCP) {
		net_dhcpv4_start(ni);
		LOG_INF("%s: DHCP", device_config_interface_str(iface));
		return 0;
	}

	struct net_in_addr addr;
	struct net_in_addr mask;

	memcpy(addr.s4_addr, ipv4->address, 4);
	net_map_netmask(ipv4->prefix_length, mask.s4_addr);
	if (net_if_ipv4_addr_add(ni, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		applied->known = false;
		return -ENOMEM;
	}
	(void)net_if_ipv4_set_netmask_by_addr(ni, &addr, &mask);
	if (ipv4->has_gateway) {
		struct net_in_addr gw;

		memcpy(gw.s4_addr, ipv4->gateway, 4);
		net_if_ipv4_set_gw(ni, &gw);
	}
	LOG_INF("%s: static /%u%s", device_config_interface_str(iface), ipv4->prefix_length,
		ipv4->has_gateway ? " with gateway" : "");

	return 0;
}

/* -- Wi-Fi ----------------------------------------------------------------------- */

static int wifi_connect(void *ctx, const uint8_t *ssid, size_t ssid_len,
			enum device_config_wifi_security security, bool hidden,
			const uint8_t *password, size_t password_len)
{
	struct wifi_connect_req_params params = {
		.ssid = ssid,
		.ssid_length = (uint8_t)ssid_len,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_UNKNOWN,
		.timeout = SYS_FOREVER_MS,
	};
	int rc;

	ARG_UNUSED(ctx);
	/*
	 * A hidden network needs nothing more here: the coprocessor probes for
	 * the SSID it is given. The flag is kept in the configuration for the
	 * form, and for a driver that one day needs it.
	 */
	ARG_UNUSED(hidden);

	if (!wifi_ready()) {
		return -ENODEV;
	}

	switch (security) {
	case DEVICE_CONFIG_WIFI_WPA2_PSK:
		params.security = WIFI_SECURITY_TYPE_PSK;
		params.psk = password;
		params.psk_length = (uint8_t)password_len;
		break;
	case DEVICE_CONFIG_WIFI_WPA3_SAE:
		params.security = WIFI_SECURITY_TYPE_SAE;
		params.sae_password = password;
		params.sae_password_length = (uint8_t)password_len;
		break;
	default:
		params.security = WIFI_SECURITY_TYPE_NONE;
		break;
	}

	if (atomic_get(&ad.associated) != 0 || atomic_get(&ad.connecting) != 0) {
		(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, ad.wifi, NULL, 0);
	}

	k_spinlock_key_t key = k_spin_lock(&ad.lock);

	ad.ssid_len = (uint8_t)MIN(ssid_len, sizeof(ad.ssid));
	memcpy(ad.ssid, ssid, ad.ssid_len);
	ad.rssi_valid = false;
	ad.rssi_at_ms = 0;
	k_spin_unlock(&ad.lock, key);

	atomic_set(&ad.associated, 0);
	atomic_set(&ad.failed, 0);
	atomic_set(&ad.connecting, 1);

	rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, ad.wifi, &params, sizeof(params));
	if (rc != 0) {
		atomic_set(&ad.connecting, 0);
		atomic_set(&ad.failed, 1);
		LOG_WRN("Wi-Fi join refused (%d)", rc);
	}

	return rc;
}

static int wifi_disconnect(void *ctx)
{
	ARG_UNUSED(ctx);

	/* Not associated, or no coprocessor to ask: already where it should be. */
	if (!wifi_ready() ||
	    (atomic_get(&ad.associated) == 0 && atomic_get(&ad.connecting) == 0)) {
		atomic_set(&ad.failed, 0);
		return 0;
	}

	int rc = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, ad.wifi, NULL, 0);

	atomic_set(&ad.associated, 0);
	atomic_set(&ad.connecting, 0);
	atomic_set(&ad.failed, 0);

	return (rc == -EALREADY) ? 0 : rc;
}

static void scan_result(struct net_if *iface, int status, struct wifi_scan_result *entry)
{
	struct network_scan_results *out = ad.scan_out;
	struct network_access_point *ap;

	ARG_UNUSED(iface);
	ARG_UNUSED(status);

	if (out == NULL || entry == NULL) {
		return;
	}
	if (out->count >= ARRAY_SIZE(out->items)) {
		out->truncated = true;
		return;
	}
	ap = &out->items[out->count++];
	ap->ssid_len = (uint8_t)MIN(entry->ssid_length, (uint8_t)DEVICE_CONFIG_SSID_MAX_LEN);
	memcpy(ap->ssid, entry->ssid, ap->ssid_len);
	memcpy(ap->bssid, entry->mac, MIN(entry->mac_length, (uint8_t)sizeof(ap->bssid)));
	ap->rssi = entry->rssi;
	ap->channel = entry->channel;
	ap->security = net_map_wifi_security(entry->security);
}

static int wifi_scan(void *ctx, struct network_scan_results *out)
{
	const struct device *dev;
	const struct net_wifi_mgmt_offload *api;
	struct wifi_scan_params params = {0};
	int rc;

	ARG_UNUSED(ctx);

	if (!wifi_ready()) {
		return -ENODEV;
	}
	/*
	 * The driver's scan operation, called directly with a callback of this
	 * adapter's: through net_mgmt each result would travel as an event whose
	 * data is capped at CONFIG_NET_MGMT_EVENT_INFO_DEFAULT_DATA_SIZE, 32 bytes,
	 * smaller than one wifi_scan_result. The ESP-Hosted scan blocks and calls
	 * back on this thread, which is the worker's.
	 */
	dev = net_if_get_device(ad.wifi);
	api = dev->api;
	if (api == NULL || api->wifi_mgmt_api == NULL || api->wifi_mgmt_api->scan == NULL) {
		return -ENOTSUP;
	}
	ad.scan_out = out;
	rc = api->wifi_mgmt_api->scan(dev, ad.wifi, &params, scan_result);
	ad.scan_out = NULL;

	return rc;
}

/* -- DNS -------------------------------------------------------------------------- */

static void to_sockaddr(const struct device_config_addr *addr, struct net_sockaddr_storage *out)
{
	memset(out, 0, sizeof(*out));
	if (addr->family == DEVICE_CONFIG_AF_INET6) {
		struct net_sockaddr_in6 *sin6 = (struct net_sockaddr_in6 *)out;

		sin6->sin6_family = NET_AF_INET6;
		sin6->sin6_port = net_htons(53);
		memcpy(sin6->sin6_addr.s6_addr, addr->bytes, 16);
	} else {
		struct net_sockaddr_in *sin = (struct net_sockaddr_in *)out;

		sin->sin_family = NET_AF_INET;
		sin->sin_port = net_htons(53);
		memcpy(sin->sin_addr.s4_addr, addr->bytes, 4);
	}
}

static int set_dns(void *ctx, const struct device_config_addr *servers, size_t count)
{
	struct dns_resolve_context *resolver = dns_resolve_get_default();
	struct net_sockaddr_storage storage[DEVICE_CONFIG_DNS_MAX_SERVERS];
	const struct net_sockaddr *list[DEVICE_CONFIG_DNS_MAX_SERVERS + 1] = {0};
	int rc = 0;

	ARG_UNUSED(ctx);
	count = MIN(count, (size_t)DEVICE_CONFIG_DNS_MAX_SERVERS);

	/*
	 * The manual list installed before goes first, by closing the context.
	 * dns_resolve_remove_server_addresses() cannot remove it: with no
	 * interfaces given it matches no server at all (idx_of_server_addr()
	 * skips every slot when if_index is 0), and a manual server has no
	 * interface to give. Board B showed the old servers still in force after
	 * a switch to automatic (docs/device-development/reports/p4/hw). The DHCP
	 * and router-advertisement servers the close would also drop were
	 * already removed when the manual list went in; the next reconfigure,
	 * ours or a lease renewal's, makes the context active again.
	 */
	if (ad.manual_dns_count > 0U) {
		rc = dns_resolve_close(resolver);
		if (rc != 0 && rc != -ENOENT) {
			LOG_WRN("removing the manual resolvers failed (%d)", rc);
		}
		rc = 0;
		ad.manual_dns_count = 0;
	}
	if (count == 0U) {
		/*
		 * Automatic: what DHCP and router advertisements provide. Servers
		 * a manual list displaced return with the next lease renewal.
		 */
		return 0;
	}

	/* Manual: the servers DHCP and router advertisements added would still be asked. */
	const struct net_if *const ifaces[] = {ad.eth, ad.wifi};

	for (size_t i = 0; i < ARRAY_SIZE(ifaces); i++) {
		if (ifaces[i] == NULL) {
			continue;
		}
		const int index = net_if_get_by_iface((struct net_if *)ifaces[i]);

		(void)dns_resolve_remove_source(resolver, index, DNS_SOURCE_DHCPV4);
		(void)dns_resolve_remove_source(resolver, index, DNS_SOURCE_DHCPV6);
		(void)dns_resolve_remove_source(resolver, index, DNS_SOURCE_IPV6_RA);
	}

	for (size_t i = 0; i < count; i++) {
		to_sockaddr(&servers[i], &storage[i]);
		list[i] = (const struct net_sockaddr *)&storage[i];
	}
	list[count] = NULL;
	rc = dns_resolve_reconfigure(resolver, NULL, list, DNS_SOURCE_MANUAL);
	if (rc == 0) {
		memcpy(ad.manual_dns, servers, count * sizeof(servers[0]));
		ad.manual_dns_count = (uint8_t)count;
	} else {
		LOG_WRN("installing resolvers failed (%d)", rc);
	}

	return rc;
}

static int get_dns(void *ctx, struct device_config_addr *out, size_t cap, size_t *count)
{
	struct dns_resolve_context *resolver = dns_resolve_get_default();
	size_t n = 0;

	ARG_UNUSED(ctx);

	/* Busy with a query: the last answer rather than a wait. */
	if (k_mutex_lock(&resolver->lock, K_NO_WAIT) != 0) {
		n = MIN(cap, (size_t)ad.last_dns_count);
		memcpy(out, ad.last_dns, n * sizeof(out[0]));
		*count = n;
		return 0;
	}
	for (size_t i = 0; i < ARRAY_SIZE(resolver->servers) && n < cap; i++) {
		const struct net_sockaddr *sa =
			(const struct net_sockaddr *)&resolver->servers[i].dns_server_addr;

		if (resolver->servers[i].is_mdns || resolver->servers[i].is_llmnr) {
			continue;
		}
		memset(&out[n], 0, sizeof(out[n]));
		if (sa->sa_family == NET_AF_INET) {
			out[n].family = DEVICE_CONFIG_AF_INET;
			memcpy(out[n].bytes, net_sin(sa)->sin_addr.s4_addr, 4);
			n++;
		} else if (sa->sa_family == NET_AF_INET6) {
			out[n].family = DEVICE_CONFIG_AF_INET6;
			memcpy(out[n].bytes, net_sin6(sa)->sin6_addr.s6_addr, 16);
			n++;
		}
	}
	k_mutex_unlock(&resolver->lock);

	ad.last_dns_count = (uint8_t)MIN(n, ARRAY_SIZE(ad.last_dns));
	memcpy(ad.last_dns, out, ad.last_dns_count * sizeof(out[0]));
	*count = n;

	return 0;
}

static int set_default(void *ctx, enum device_config_interface iface)
{
	struct net_if *ni = pick(iface);

	ARG_UNUSED(ctx);

	if (ni == NULL) {
		return -ENODEV;
	}
	net_if_set_default(ni);

	return 0;
}

const struct network_iface_ops net_adapter_ops = {
	.configure = configure,
	.wifi_connect = wifi_connect,
	.wifi_disconnect = wifi_disconnect,
	.get_status = get_status,
	.set_dns = set_dns,
	.get_dns = get_dns,
	.set_default = set_default,
	.wifi_scan = wifi_scan,
	.ctx = NULL,
};

int net_adapter_init(void (*kick)(void))
{
	ad.kick = kick;
	ad.eth = net_if_lookup_by_dev(DEVICE_DT_GET_ONE(wiznet_w5500));
	ad.wifi = net_if_get_wifi_sta();

	net_mgmt_init_event_callback(&iface_cb, on_event, NET_EVENT_IF_UP | NET_EVENT_IF_DOWN);
	net_mgmt_add_event_callback(&iface_cb);
	net_mgmt_init_event_callback(&ipv4_cb, on_event,
				     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL |
					     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&ipv4_cb);
	net_mgmt_init_event_callback(&ipv6_cb, on_event,
				     NET_EVENT_IPV6_ADDR_ADD | NET_EVENT_IPV6_ADDR_DEL);
	net_mgmt_add_event_callback(&ipv6_cb);
	net_mgmt_init_event_callback(&wifi_cb, on_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	LOG_INF("Ethernet %s, Wi-Fi coprocessor %s", ad.eth != NULL ? "found" : "missing",
		ad.wifi == NULL ? "missing" : (wifi_ready() ? "ready" : "not responding"));

	return (ad.eth != NULL) ? 0 : -ENODEV;
}
