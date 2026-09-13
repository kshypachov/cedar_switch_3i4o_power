/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Validation of a proposed network configuration, and the policies that read
 * the interfaces without changing them.
 *
 * Split out from the state machine because it is the half with no state: given
 * a proposal, the committed configuration and what the interfaces are doing
 * right now, it either produces field errors or it does not. That makes it
 * readable on its own, and it is where almost every rule from section 5 of the
 * plan and the "Сеть" section of the contract actually lives.
 *
 * Every rejection names a JSON Pointer into the request body, so a form can
 * mark the field rather than showing one sentence about the whole request.
 * Paths, codes and the order they are added in are the mock's
 * (tools/api-contract/cedar_contract/mock/network.py, Network._validate): the
 * error keeps the first CONFIG_API_VALIDATION_MAX_FIELDS entries, so the order
 * decides which fields a client is told about.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "network_internal.h"

/* Paths into NetworkTransactionRequest. Written out rather than built at
 * runtime: they are part of the API surface, and a typo in one is a typo a
 * client sees.
 */
#define P_PREFERRED "/config/preferred_interface"
#define P_DNS_SERVERS "/config/dns/servers"
#define P_ETH "/config/interfaces/ethernet"
#define P_WIFI "/config/interfaces/wifi"

static const char *iface_path(enum device_config_interface iface)
{
	return (iface == DEVICE_CONFIG_INTERFACE_ETHERNET) ? P_ETH : P_WIFI;
}

/*
 * Builds "<interface>/ipv4/<field>" into a caller-supplied buffer. A path that
 * does not fit is dropped by api_error_add_field() rather than truncated, so
 * the buffer is sized for the longest path this file can produce.
 */
static void ipv4_path(char *buf, size_t cap, enum device_config_interface iface,
		      const char *field)
{
	const char *base = iface_path(iface);
	size_t pos = 0;

	for (const char *s = base; *s != '\0' && pos + 1U < cap; s++) {
		buf[pos++] = *s;
	}
	for (const char *s = "/ipv4/"; *s != '\0' && pos + 1U < cap; s++) {
		buf[pos++] = *s;
	}
	for (const char *s = field; *s != '\0' && pos + 1U < cap; s++) {
		buf[pos++] = *s;
	}
	buf[pos] = '\0';
}

static void add_ipv4_field(struct api_error *err, enum device_config_interface iface,
			   const char *field, enum api_field_code code)
{
	char path[96];

	ipv4_path(path, sizeof(path), iface, field);
	(void)api_error_add_field(err, path, code);
}

static bool addr_is_zero(const uint8_t *addr, size_t len)
{
	uint8_t any = 0U;

	for (size_t i = 0; i < len; i++) {
		any |= addr[i];
	}
	return any == 0U;
}

/*
 * One interface's IPv4 block.
 *
 * The DHCP branch is as strict as the static one on purpose. The contract says
 * DHCP requires the static fields to be null, and accepting a leftover address
 * beside mode=dhcp would mean the stored configuration no longer says what the
 * operator asked for — the next person to read it back would see an address
 * that is not in use and has no way to tell.
 */
static bool validate_ipv4(const struct device_config_ipv4 *ipv4, bool address_given,
			  enum device_config_interface iface, struct api_error *err)
{
	bool ok = true;
	const bool has_address = address_given || !addr_is_zero(ipv4->address, 4);

	if (ipv4->mode >= DEVICE_CONFIG_IPV4_MODE_COUNT) {
		add_ipv4_field(err, iface, "mode", API_FIELD_INVALID_FORMAT);
		return false;
	}

	if (ipv4->mode == DEVICE_CONFIG_IPV4_DHCP) {
		if (has_address) {
			add_ipv4_field(err, iface, "address", API_FIELD_NOT_ALLOWED);
			ok = false;
		}
		if (ipv4->prefix_length != 0U) {
			add_ipv4_field(err, iface, "prefix_length", API_FIELD_NOT_ALLOWED);
			ok = false;
		}
		if (ipv4->has_gateway) {
			add_ipv4_field(err, iface, "gateway", API_FIELD_NOT_ALLOWED);
			ok = false;
		}
		return ok;
	}

	/* Static. The schema fixes 1-30; zero is what null decodes to. */
	if (!has_address) {
		add_ipv4_field(err, iface, "address", API_FIELD_REQUIRED);
		ok = false;
	}
	if (ipv4->prefix_length == 0U) {
		add_ipv4_field(err, iface, "prefix_length", API_FIELD_REQUIRED);
		ok = false;
	} else if (!api_ipv4_prefix_is_valid(ipv4->prefix_length)) {
		add_ipv4_field(err, iface, "prefix_length", API_FIELD_OUT_OF_RANGE);
		ok = false;
	}
	if (!ok) {
		return false; /* Nothing below can be judged without both. */
	}

	/*
	 * Usable-host is checked rather than merely well-formed. A subnet or
	 * broadcast address, or one from 127/8 or 169.254/16, is accepted by
	 * the kernel and then does not work — the operator would see a device
	 * that took the setting and went quiet. A gateway is not judged against
	 * an address that is already wrong: the second message would be noise.
	 */
	if (!api_ipv4_is_usable_host(ipv4->address, ipv4->prefix_length)) {
		add_ipv4_field(err, iface, "address", API_FIELD_OUT_OF_RANGE);
		return false;
	}

	if (ipv4->has_gateway) {
		if (!api_ipv4_is_usable_host(ipv4->gateway, ipv4->prefix_length) ||
		    !api_ipv4_same_subnet(ipv4->address, ipv4->gateway, ipv4->prefix_length)) {
			/*
			 * Off the subnet it is unreachable without a route that
			 * does not exist yet, so it silently does nothing.
			 */
			add_ipv4_field(err, iface, "gateway", API_FIELD_OUT_OF_RANGE);
			ok = false;
		} else if (memcmp(ipv4->address, ipv4->gateway, 4) == 0) {
			add_ipv4_field(err, iface, "gateway", API_FIELD_CONFLICTING);
			ok = false;
		}
	}

	return ok;
}

static void add_server_field(struct api_error *err, uint8_t index, enum api_field_code code)
{
	char path[sizeof(P_DNS_SERVERS) + 4];
	size_t pos = sizeof(P_DNS_SERVERS) - 1U;

	memcpy(path, P_DNS_SERVERS, pos);
	path[pos++] = '/';
	if (index >= 10U) {
		path[pos++] = (char)('0' + index / 10U);
	}
	path[pos++] = (char)('0' + index % 10U);
	path[pos] = '\0';
	(void)api_error_add_field(err, path, code);
}

static bool validate_dns(const struct device_config_dns *dns, struct api_error *err)
{
	bool ok = true;

	if (dns->mode >= DEVICE_CONFIG_DNS_MODE_COUNT) {
		(void)api_error_add_field(err, "/config/dns/mode", API_FIELD_INVALID_FORMAT);
		return false;
	}

	if (dns->mode == DEVICE_CONFIG_DNS_AUTOMATIC && dns->server_count != 0U) {
		(void)api_error_add_field(err, P_DNS_SERVERS, API_FIELD_NOT_ALLOWED);
		ok = false;
	}
	/*
	 * Manual with no servers would leave the device with no resolver at
	 * all, which is a different thing from automatic and almost certainly
	 * not what was meant.
	 */
	if (dns->mode == DEVICE_CONFIG_DNS_MANUAL && dns->server_count == 0U) {
		(void)api_error_add_field(err, P_DNS_SERVERS, API_FIELD_REQUIRED);
		ok = false;
	}
	if (dns->server_count > DEVICE_CONFIG_DNS_MAX_SERVERS) {
		/* The schema's maxItems, for a caller that is not the schema. */
		(void)api_error_add_field(err, P_DNS_SERVERS, API_FIELD_OUT_OF_RANGE);
		return false;
	}

	for (uint8_t i = 0; i < dns->server_count; i++) {
		const struct device_config_addr *s = &dns->servers[i];
		const bool known = s->family == DEVICE_CONFIG_AF_INET ||
				   s->family == DEVICE_CONFIG_AF_INET6;

		/* An all-zero resolver is the unspecified address, never a server. */
		if (!known ||
		    addr_is_zero(s->bytes, s->family == DEVICE_CONFIG_AF_INET ? 4U : 16U)) {
			add_server_field(err, i, API_FIELD_INVALID_FORMAT);
			ok = false;
		}
	}

	return ok;
}

/*
 * Wi-Fi, including the credential rules the contract states in prose.
 *
 * The subtle one is the last: keeping the stored password while changing SSID
 * or security is refused. The stored password belongs to the network it was
 * entered for, and carrying it to a different SSID would produce a profile
 * that silently cannot associate, with nothing in the API to show why.
 */
static bool validate_wifi(const struct network_config_input *input,
			  const struct device_config *committed, struct api_error *err)
{
	const struct device_config_wifi *wifi = &input->config.wifi;
	const struct device_config_secret_update *credential = &input->wifi_password;
	bool ok = true;

	if (!wifi->enabled) {
		/*
		 * A disabled interface is not held to the rest. Its settings
		 * are kept so that re-enabling restores the profile rather
		 * than presenting an empty form.
		 */
		return true;
	}

	if (input->ssid_invalid) {
		(void)api_error_add_field(err, P_WIFI "/ssid_base64", API_FIELD_INVALID_FORMAT);
		ok = false;
	} else if (wifi->ssid_len == 0U) {
		(void)api_error_add_field(err, P_WIFI "/ssid_base64", API_FIELD_REQUIRED);
		ok = false;
	} else if (wifi->ssid_len > DEVICE_CONFIG_SSID_MAX_LEN) {
		(void)api_error_add_field(err, P_WIFI "/ssid_base64", API_FIELD_OUT_OF_RANGE);
		ok = false;
	}

	if (wifi->security >= DEVICE_CONFIG_WIFI_SECURITY_COUNT) {
		(void)api_error_add_field(err, P_WIFI "/security", API_FIELD_INVALID_FORMAT);
		return false;
	}

	const bool same_ssid = !input->ssid_invalid && committed->wifi.ssid_len == wifi->ssid_len &&
			       wifi->ssid_len <= DEVICE_CONFIG_SSID_MAX_LEN &&
			       memcmp(committed->wifi.ssid, wifi->ssid, wifi->ssid_len) == 0;
	const bool same_security = committed->wifi.security == wifi->security;

	switch (credential->action) {
	case DEVICE_CONFIG_SECRET_REPLACE:
		/*
		 * A replace with no value, or one too long to store, is the
		 * schema's oneOf failing, which the decoder reports on the
		 * credential object as a whole.
		 */
		if (credential->value == NULL || credential->len == 0U ||
		    credential->len > DEVICE_CONFIG_SECRET_MAX_LEN) {
			(void)api_error_add_field(err, P_WIFI "/credential", API_FIELD_CONFLICTING);
			ok = false;
		} else if (wifi->security == DEVICE_CONFIG_WIFI_OPEN) {
			/*
			 * The contract clears an open network's password
			 * explicitly rather than storing one that is never
			 * used, so a password offered here is a contradiction,
			 * not something to quietly drop.
			 */
			(void)api_error_add_field(err, P_WIFI "/credential/action",
						  API_FIELD_NOT_ALLOWED);
			ok = false;
		}
		break;

	case DEVICE_CONFIG_SECRET_CLEAR:
		if (wifi->security != DEVICE_CONFIG_WIFI_OPEN) {
			(void)api_error_add_field(err, P_WIFI "/credential/action",
						  API_FIELD_CONFLICTING);
			ok = false;
		}
		break;

	case DEVICE_CONFIG_SECRET_KEEP:
		if (wifi->security != DEVICE_CONFIG_WIFI_OPEN) {
			if (!committed->wifi.password_set) {
				(void)api_error_add_field(err, P_WIFI "/credential/action",
							  API_FIELD_CONFLICTING);
				ok = false;
			} else if (!same_ssid || !same_security) {
				(void)api_error_add_field(err, P_WIFI "/credential/action",
							  API_FIELD_NOT_ALLOWED);
				ok = false;
			}
		}
		break;

	default:
		(void)api_error_add_field(err, P_WIFI "/credential", API_FIELD_CONFLICTING);
		ok = false;
		break;
	}

	return ok;
}

static bool interface_enabled(const struct device_config *cfg, enum device_config_interface iface)
{
	return (iface == DEVICE_CONFIG_INTERFACE_ETHERNET) ? cfg->ethernet.enabled
							    : cfg->wifi.enabled;
}

bool network_validate_config(const struct network_config_input *input,
			     const struct device_config *committed,
			     const struct network_status *status, struct api_error *err)
{
	const struct device_config *cfg = &input->config;
	bool ok = true;

	/*
	 * Both interfaces off would leave nothing to manage the device with,
	 * and no API call could turn one back on.
	 */
	if (!cfg->ethernet.enabled && !cfg->wifi.enabled) {
		(void)api_error_add_field(err, P_ETH "/enabled", API_FIELD_CONFLICTING);
		(void)api_error_add_field(err, P_WIFI "/enabled", API_FIELD_CONFLICTING);
		ok = false;
	} else if (cfg->wifi.enabled && !status->wifi.present) {
		/*
		 * Device-only in substance (the mock stands the coprocessor's
		 * state in for it). Confirm waits for every enabled interface,
		 * so a Wi-Fi that cannot come up would make this change — and
		 * every later one that keeps it enabled — impossible to confirm.
		 */
		(void)api_error_add_field(err, P_WIFI "/enabled", API_FIELD_NOT_ALLOWED);
		ok = false;
	}

	if (!validate_ipv4(&cfg->ethernet.ipv4, input->ethernet_address_given,
			   DEVICE_CONFIG_INTERFACE_ETHERNET, err)) {
		ok = false;
	}
	if (!validate_ipv4(&cfg->wifi.ipv4, input->wifi_address_given,
			   DEVICE_CONFIG_INTERFACE_WIFI, err)) {
		ok = false;
	}
	if (!validate_dns(&cfg->dns, err)) {
		ok = false;
	}

	if (cfg->preferred_interface >= DEVICE_CONFIG_INTERFACE_COUNT) {
		(void)api_error_add_field(err, P_PREFERRED, API_FIELD_INVALID_FORMAT);
		ok = false;
	} else if (!interface_enabled(cfg, cfg->preferred_interface)) {
		(void)api_error_add_field(err, P_PREFERRED, API_FIELD_CONFLICTING);
		ok = false;
	}

	if (!validate_wifi(input, committed, err)) {
		ok = false;
	}

	/*
	 * Device-only: the recovery path from step 1 of section 5. At least one
	 * interface that stays enabled must have a link right now — otherwise
	 * the change is being applied over a path that does not exist, and there
	 * would be no way for anyone to tell the device to undo it.
	 *
	 * Link rather than address: the address is exactly what is about to
	 * change, so requiring the new one to already be working would refuse
	 * every valid static configuration.
	 */
	if (ok) {
		const bool eth_path = cfg->ethernet.enabled && status->ethernet.present &&
				      status->ethernet.link_up;
		const bool wifi_path =
			cfg->wifi.enabled && status->wifi.present && status->wifi.link_up;

		if (!eth_path && !wifi_path) {
			(void)api_error_add_field(err, P_ETH "/enabled", API_FIELD_NOT_ALLOWED);
			(void)api_error_add_field(err, P_WIFI "/enabled", API_FIELD_NOT_ALLOWED);
			ok = false;
		}
	}

	return ok;
}

/*
 * The health check behind confirm, and the reason a client reaching the
 * endpoint is not enough on its own: that proves the client's own path works
 * and says nothing about the other interface.
 */
bool network_config_is_healthy(const struct device_config *cfg,
			       const struct network_status *status)
{
	const struct {
		const struct device_config_ipv4 *ipv4;
		const struct network_iface_status *st;
		bool enabled;
	} checks[] = {
		{&cfg->ethernet.ipv4, &status->ethernet, cfg->ethernet.enabled},
		{&cfg->wifi.ipv4, &status->wifi, cfg->wifi.enabled},
	};

	for (size_t i = 0; i < ARRAY_SIZE(checks); i++) {
		if (!checks[i].enabled) {
			continue;
		}

		const struct network_iface_status *st = checks[i].st;

		if (!st->present || !st->link_up || !st->has_ipv4) {
			return false;
		}
		/*
		 * A configuration that asked for a gateway is not working
		 * until the route exists: link and address alone would let a
		 * device with no way off its own subnet look healthy.
		 */
		if (checks[i].ipv4->mode == DEVICE_CONFIG_IPV4_STATIC &&
		    checks[i].ipv4->has_gateway && !st->has_route) {
			return false;
		}
	}

	return true;
}

enum network_iface_state network_iface_state_of(enum device_config_interface iface,
						const struct network_iface_status *st,
						bool enabled)
{
	if (!enabled) {
		return NETWORK_IFACE_DISABLED;
	}
	if (!st->present) {
		return NETWORK_IFACE_FAILED;
	}
	if (iface == DEVICE_CONFIG_INTERFACE_WIFI && !st->wifi_associated) {
		/*
		 * A failed attempt stays failed until the next one starts: the
		 * operator has to see that the password or the network was
		 * wrong, not an interface that looks merely idle.
		 */
		if (st->wifi_connecting) {
			return NETWORK_IFACE_CONNECTING;
		}
		return st->wifi_failed ? NETWORK_IFACE_FAILED : NETWORK_IFACE_DOWN;
	}
	if (!st->link_up) {
		return NETWORK_IFACE_DOWN;
	}
	return st->has_ipv4 ? NETWORK_IFACE_READY : NETWORK_IFACE_ADDRESSING;
}

static bool can_carry_traffic(const struct device_config *cfg, const struct network_status *status,
			      enum device_config_interface iface)
{
	const struct network_iface_status *st = (iface == DEVICE_CONFIG_INTERFACE_ETHERNET)
							? &status->ethernet
							: &status->wifi;

	return interface_enabled(cfg, iface) && st->present && st->link_up && st->has_ipv4;
}

bool network_select_default(const struct device_config *cfg, const struct network_status *status,
			    enum device_config_interface *out)
{
	/*
	 * Section 5's policy: Ethernet preferred, Wi-Fi fallback — or whichever
	 * the configuration prefers. Judged by link and address only, like the
	 * health check: no ping to a public host decides which way traffic goes.
	 */
	const enum device_config_interface preferred =
		(cfg->preferred_interface == DEVICE_CONFIG_INTERFACE_WIFI)
			? DEVICE_CONFIG_INTERFACE_WIFI
			: DEVICE_CONFIG_INTERFACE_ETHERNET;
	const enum device_config_interface other = (preferred == DEVICE_CONFIG_INTERFACE_WIFI)
							   ? DEVICE_CONFIG_INTERFACE_ETHERNET
							   : DEVICE_CONFIG_INTERFACE_WIFI;

	if (can_carry_traffic(cfg, status, preferred)) {
		*out = preferred;
		return true;
	}
	if (can_carry_traffic(cfg, status, other)) {
		*out = other;
		return true;
	}
	return false;
}
