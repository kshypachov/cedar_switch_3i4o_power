/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See fake_iface.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "fake_iface.h"

/*
 * Derive what an interface would actually end up with, given what it was told
 * and what the test says the segment is doing. This is the part that makes the
 * health check worth testing: a static address appears immediately, a DHCP one
 * only if a server answers, and neither appears at all with the link down.
 */
static void settle(struct fake_iface *fi)
{
	fi->has_ipv4 = false;
	fi->has_route = false;
	fi->prefix_length = 0;
	memset(fi->ipv4, 0, sizeof(fi->ipv4));

	if (!fi->present || !fi->configured || !fi->enabled || !fi->link_up) {
		return;
	}

	if (fi->applied.mode == DEVICE_CONFIG_IPV4_STATIC) {
		memcpy(fi->ipv4, fi->applied.address, sizeof(fi->ipv4));
		fi->prefix_length = fi->applied.prefix_length;
		fi->has_ipv4 = true;
		fi->has_route = fi->applied.has_gateway && !fi->suppress_route;
		return;
	}

	if (fi->dhcp_answers) {
		/* A lease from the segment the board actually lives on. */
		fi->ipv4[0] = 192;
		fi->ipv4[1] = 168;
		fi->ipv4[2] = 88;
		fi->ipv4[3] = 14;
		fi->prefix_length = 24;
		fi->has_ipv4 = true;
		fi->has_route = !fi->suppress_route;
	}
}

static struct fake_iface *pick(struct fake_net *fn, enum device_config_interface iface)
{
	return (iface == DEVICE_CONFIG_INTERFACE_ETHERNET) ? &fn->eth : &fn->wifi;
}

static void during_io(struct fake_net *fn)
{
	void (*hook)(void *arg) = fn->during_io;

	fn->during_io = NULL;
	if (hook != NULL) {
		hook(fn->during_io_arg);
	}
}

static int fake_configure(void *ctx, enum device_config_interface iface,
			  const struct device_config_ipv4 *ipv4, bool enabled)
{
	struct fake_net *fn = ctx;

	during_io(fn);

	if (fn->fail_configure != 0) {
		int err = fn->fail_configure;

		fn->fail_configure = 0;
		return err;
	}

	struct fake_iface *fi = pick(fn, iface);

	fi->configure_calls++;
	fi->configured = true;
	fi->enabled = enabled;
	fi->applied = *ipv4;
	settle(fi);

	return 0;
}

static int fake_wifi_connect(void *ctx, const uint8_t *ssid, size_t ssid_len,
			     enum device_config_wifi_security security, bool hidden,
			     const uint8_t *password, size_t password_len)
{
	struct fake_net *fn = ctx;

	ARG_UNUSED(security);
	ARG_UNUSED(hidden);

	during_io(fn);
	fn->connect_calls++;

	if (fn->fail_connect != 0) {
		int err = fn->fail_connect;

		fn->fail_connect = 0;
		return err;
	}
	if (!fn->wifi.present) {
		return -ENODEV;
	}

	fn->ssid_len = (uint8_t)MIN(ssid_len, sizeof(fn->ssid));
	memcpy(fn->ssid, ssid, fn->ssid_len);

	fn->password_len = MIN(password_len, sizeof(fn->password));
	memset(fn->password, 0, sizeof(fn->password));
	if (password != NULL) {
		memcpy(fn->password, password, fn->password_len);
	}

	if (fn->connect_pending) {
		fn->connecting = true;
		fn->associated = false;
		fn->connect_failed = false;
	} else {
		fn->connecting = false;
		fn->associated = fn->wifi.wifi_associates;
		fn->connect_failed = !fn->associated;
	}

	/*
	 * Association is what brings the radio's link up, which is why a wrong
	 * password shows as an interface that was configured and still has no
	 * address rather than as a failed call.
	 */
	fn->wifi.link_up = fn->associated;
	settle(&fn->wifi);

	return 0;
}

static int fake_wifi_disconnect(void *ctx)
{
	struct fake_net *fn = ctx;

	during_io(fn);
	fn->disconnect_calls++;
	fn->associated = false;
	fn->connecting = false;
	fn->connect_failed = false;
	fn->ssid_len = 0;
	fn->wifi.link_up = false;
	settle(&fn->wifi);

	return 0;
}

static int fake_get_status(void *ctx, enum device_config_interface iface,
			   struct network_iface_status *out)
{
	struct fake_net *fn = ctx;
	struct fake_iface *fi = pick(fn, iface);

	settle(fi);
	memset(out, 0, sizeof(*out));

	out->present = fi->present;
	out->enabled = fi->enabled;
	out->link_up = fi->present && fi->link_up;
	out->has_ipv4 = fi->has_ipv4;
	memcpy(out->ipv4, fi->ipv4, sizeof(out->ipv4));
	out->prefix_length = fi->prefix_length;
	out->has_gateway = fi->applied.has_gateway;
	memcpy(out->gateway, fi->applied.gateway, sizeof(out->gateway));
	out->has_route = fi->has_route;
	memcpy(out->mac, fi->mac, sizeof(out->mac));

	if (fi->has_ipv4) {
		struct network_addr *a = &out->addrs[out->addr_count++];

		a->family = DEVICE_CONFIG_AF_INET;
		a->prefix_length = fi->prefix_length;
		a->source = (fi->applied.mode == DEVICE_CONFIG_IPV4_STATIC) ? NETWORK_ADDR_STATIC
									    : NETWORK_ADDR_DHCP;
		memcpy(a->bytes, fi->ipv4, 4);
	}
	for (uint8_t i = 0; i < fi->extra_count && out->addr_count < NETWORK_IFACE_MAX_ADDRS;
	     i++) {
		out->addrs[out->addr_count++] = fi->extra[i];
	}

	if (iface == DEVICE_CONFIG_INTERFACE_WIFI) {
		out->wifi_associated = fn->associated;
		out->wifi_connecting = fn->connecting;
		out->wifi_failed = fn->connect_failed;
		out->ssid_len = fn->ssid_len;
		memcpy(out->ssid, fn->ssid, fn->ssid_len);
		out->rssi = fn->rssi;
		out->rssi_valid = fn->associated;
	}

	return 0;
}

static int fake_set_dns(void *ctx, const struct device_config_addr *servers, size_t count)
{
	struct fake_net *fn = ctx;

	during_io(fn);
	fn->set_dns_calls++;
	fn->dns_count = (uint8_t)MIN(count, (size_t)DEVICE_CONFIG_DNS_MAX_SERVERS);
	memset(fn->dns, 0, sizeof(fn->dns));
	if (servers != NULL && fn->dns_count > 0U) {
		memcpy(fn->dns, servers, sizeof(fn->dns[0]) * fn->dns_count);
	}
	/* A fresh install is what the resolver reports until something replaces it. */
	fn->dns_override = false;

	return 0;
}

static int fake_get_dns(void *ctx, struct device_config_addr *out, size_t cap, size_t *count)
{
	struct fake_net *fn = ctx;
	const struct device_config_addr *src = fn->dns_override ? fn->dns_seen : fn->dns;
	const size_t n = MIN(cap, (size_t)(fn->dns_override ? fn->dns_seen_count : fn->dns_count));

	memcpy(out, src, sizeof(out[0]) * n);
	*count = n;

	return 0;
}

static int fake_set_default(void *ctx, enum device_config_interface iface)
{
	struct fake_net *fn = ctx;

	fn->set_default_calls++;
	fn->has_default = true;
	fn->default_iface = iface;

	return 0;
}

static int fake_wifi_scan(void *ctx, struct network_scan_results *out)
{
	struct fake_net *fn = ctx;

	during_io(fn);
	fn->scan_calls++;

	if (fn->fail_scan != 0) {
		int err = fn->fail_scan;

		fn->fail_scan = 0;
		return err;
	}

	*out = fn->scan_results;

	return 0;
}

void fake_net_init(struct fake_net *fn)
{
	memset(fn, 0, sizeof(*fn));

	/* A plausible board: cable in, DHCP working, C6 fitted but idle. */
	fn->eth.present = true;
	fn->eth.link_up = true;
	fn->eth.dhcp_answers = true;
	fn->eth.mac[0] = 0x80;
	fn->eth.mac[1] = 0x34;
	fn->eth.mac[2] = 0x28;
	fn->eth.mac[3] = 0x10;
	fn->eth.mac[4] = 0x6a;
	fn->eth.mac[5] = 0x1d;

	fn->wifi.present = true;
	fn->wifi.link_up = false;
	fn->wifi.dhcp_answers = true;
	fn->wifi.wifi_associates = true;
	fn->rssi = -55;
}

void fake_net_bind(struct fake_net *fn, struct network_iface_ops *ops)
{
	memset(ops, 0, sizeof(*ops));

	ops->configure = fake_configure;
	ops->wifi_connect = fake_wifi_connect;
	ops->wifi_disconnect = fake_wifi_disconnect;
	ops->get_status = fake_get_status;
	ops->set_dns = fake_set_dns;
	ops->get_dns = fn->no_get_dns ? NULL : fake_get_dns;
	ops->set_default = fn->no_set_default ? NULL : fake_set_default;
	ops->wifi_scan = fn->no_radio ? NULL : fake_wifi_scan;
	ops->ctx = fn;
}
