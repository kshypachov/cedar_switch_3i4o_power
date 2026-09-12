/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Validation of a proposed network configuration.
 *
 * Split out from the state machine because it is the half with no state: given
 * a proposal, the committed configuration and what the interfaces are doing
 * right now, it either produces field errors or it does not. That makes it
 * readable on its own, and it is where almost every rule from section 5 of the
 * plan and the "Сеть" section of the contract actually lives.
 *
 * Every rejection names a JSON Pointer into the request body, so a form can
 * mark the field rather than showing one sentence about the whole request.
 * Paths here must match the NetworkConfigInput schema in openapi.json.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "network_internal.h"

/* Paths into NetworkConfigInput. Written out rather than built at runtime:
 * they are part of the API surface, and a typo in one is a typo a client sees.
 */
#define P_PREFERRED "/preferred_interface"
#define P_DNS_MODE "/dns/mode"
#define P_DNS_SERVERS "/dns/servers"
#define P_ETH "/interfaces/ethernet"
#define P_WIFI "/interfaces/wifi"

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

static bool addr_is_zero(const uint8_t addr[4])
{
	return (addr[0] | addr[1] | addr[2] | addr[3]) == 0U;
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
static bool validate_ipv4(const struct device_config_ipv4 *ipv4,
			  enum device_config_interface iface, struct api_error *err)
{
	bool ok = true;

	if (ipv4->mode >= DEVICE_CONFIG_IPV4_MODE_COUNT) {
		add_ipv4_field(err, iface, "mode", API_FIELD_INVALID_FORMAT);
		return false;
	}

	if (ipv4->mode == DEVICE_CONFIG_IPV4_DHCP) {
		if (ipv4->prefix_length != 0U) {
			add_ipv4_field(err, iface, "prefix_length", API_FIELD_CONFLICTING);
			ok = false;
		}
		if (!addr_is_zero(ipv4->address)) {
			add_ipv4_field(err, iface, "address", API_FIELD_CONFLICTING);
			ok = false;
		}
		if (ipv4->has_gateway) {
			add_ipv4_field(err, iface, "gateway", API_FIELD_CONFLICTING);
			ok = false;
		}
		return ok;
	}

	/* Static. */
	if (!api_ipv4_prefix_is_valid(ipv4->prefix_length)) {
		add_ipv4_field(err, iface, "prefix_length",
			       ipv4->prefix_length == 0U ? API_FIELD_REQUIRED
							 : API_FIELD_OUT_OF_RANGE);
		ok = false;
	}

	if (addr_is_zero(ipv4->address)) {
		add_ipv4_field(err, iface, "address", API_FIELD_REQUIRED);
		return false; /* Nothing below can be judged without an address. */
	}

	/*
	 * Usable-host is checked rather than merely well-formed. A subnet or
	 * broadcast address, or one from 127/8 or 169.254/16, is accepted by
	 * the kernel and then does not work — the operator would see a device
	 * that took the setting and went quiet.
	 */
	if (ok && !api_ipv4_is_usable_host(ipv4->address, ipv4->prefix_length)) {
		add_ipv4_field(err, iface, "address", API_FIELD_OUT_OF_RANGE);
		ok = false;
	}

	if (ipv4->has_gateway) {
		if (!ok) {
			/* Judging the gateway against a bad prefix is noise. */
			return false;
		}
		if (!api_ipv4_is_usable_host(ipv4->gateway, ipv4->prefix_length)) {
			add_ipv4_field(err, iface, "gateway", API_FIELD_OUT_OF_RANGE);
			ok = false;
		} else if (!api_ipv4_same_subnet(ipv4->address, ipv4->gateway,
						 ipv4->prefix_length)) {
			/*
			 * A gateway off-subnet is unreachable without a route
			 * that does not exist yet, so it silently does nothing.
			 */
			add_ipv4_field(err, iface, "gateway", API_FIELD_CONFLICTING);
			ok = false;
		} else if (memcmp(ipv4->address, ipv4->gateway, 4) == 0) {
			add_ipv4_field(err, iface, "gateway", API_FIELD_CONFLICTING);
			ok = false;
		}
	}

	return ok;
}

static bool validate_dns(const struct device_config_dns *dns, struct api_error *err)
{
	bool ok = true;

	if (dns->mode >= DEVICE_CONFIG_DNS_MODE_COUNT) {
		(void)api_error_add_field(err, P_DNS_MODE, API_FIELD_INVALID_FORMAT);
		return false;
	}

	if (dns->mode == DEVICE_CONFIG_DNS_AUTOMATIC) {
		if (dns->server_count != 0U) {
			(void)api_error_add_field(err, P_DNS_SERVERS, API_FIELD_CONFLICTING);
			ok = false;
		}
		return ok;
	}

	/*
	 * Manual with no servers would leave the device with no resolver at
	 * all, which is a different thing from automatic and almost certainly
	 * not what was meant.
	 */
	if (dns->server_count == 0U) {
		(void)api_error_add_field(err, P_DNS_SERVERS, API_FIELD_REQUIRED);
		return false;
	}
	if (dns->server_count > DEVICE_CONFIG_DNS_MAX_SERVERS) {
		(void)api_error_add_field(err, P_DNS_SERVERS, API_FIELD_TOO_LONG);
		return false;
	}

	for (uint8_t i = 0; i < dns->server_count; i++) {
		const struct device_config_addr *s = &dns->servers[i];

		if (s->family != DEVICE_CONFIG_AF_INET && s->family != DEVICE_CONFIG_AF_INET6) {
			(void)api_error_add_field(err, P_DNS_SERVERS, API_FIELD_INVALID_FORMAT);
			ok = false;
			continue;
		}
		/* An all-zero resolver is the unspecified address, never a server. */
		size_t len = (s->family == DEVICE_CONFIG_AF_INET) ? 4U : 16U;
		bool zero = true;

		for (size_t b = 0; b < len; b++) {
			if (s->bytes[b] != 0U) {
				zero = false;
				break;
			}
		}
		if (zero) {
			(void)api_error_add_field(err, P_DNS_SERVERS, API_FIELD_INVALID_FORMAT);
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
static bool validate_wifi(const struct device_config_wifi *wifi,
			  const struct device_config_secret_update *credential,
			  const struct device_config *committed, struct api_error *err)
{
	bool ok = true;

	if (wifi->security >= DEVICE_CONFIG_WIFI_SECURITY_COUNT) {
		(void)api_error_add_field(err, P_WIFI "/security", API_FIELD_INVALID_FORMAT);
		ok = false;
	}
	if (wifi->ssid_len > DEVICE_CONFIG_SSID_MAX_LEN) {
		(void)api_error_add_field(err, P_WIFI "/ssid_base64", API_FIELD_TOO_LONG);
		return false;
	}

	if (!validate_ipv4(&wifi->ipv4, DEVICE_CONFIG_INTERFACE_WIFI, err)) {
		ok = false;
	}

	if (!wifi->enabled) {
		/*
		 * A disabled interface is not held to the rest. Its settings
		 * are kept so that re-enabling restores the profile rather
		 * than presenting an empty form.
		 */
		return ok;
	}

	if (wifi->ssid_len == 0U) {
		(void)api_error_add_field(err, P_WIFI "/ssid_base64", API_FIELD_REQUIRED);
		ok = false;
	}

	if (!ok) {
		return false;
	}

	const bool same_ssid = committed->wifi.ssid_len == wifi->ssid_len &&
			       memcmp(committed->wifi.ssid, wifi->ssid, wifi->ssid_len) == 0;
	const bool same_security = committed->wifi.security == wifi->security;

	switch (credential->action) {
	case DEVICE_CONFIG_SECRET_REPLACE:
		if (wifi->security == DEVICE_CONFIG_WIFI_OPEN) {
			/*
			 * The contract clears an open network's password
			 * explicitly rather than storing one that is never
			 * used, so a password offered here is a contradiction,
			 * not something to quietly drop.
			 */
			(void)api_error_add_field(err, P_WIFI "/credential",
						  API_FIELD_CONFLICTING);
			ok = false;
		}
		if (credential->value == NULL || credential->len == 0U) {
			(void)api_error_add_field(err, P_WIFI "/credential/value",
						  API_FIELD_REQUIRED);
			ok = false;
		} else if (credential->len > DEVICE_CONFIG_SECRET_MAX_LEN) {
			(void)api_error_add_field(err, P_WIFI "/credential/value",
						  API_FIELD_TOO_LONG);
			ok = false;
		}
		break;

	case DEVICE_CONFIG_SECRET_CLEAR:
		if (wifi->security != DEVICE_CONFIG_WIFI_OPEN) {
			(void)api_error_add_field(err, P_WIFI "/credential",
						  API_FIELD_CONFLICTING);
			ok = false;
		}
		break;

	case DEVICE_CONFIG_SECRET_KEEP:
		if (wifi->security != DEVICE_CONFIG_WIFI_OPEN) {
			if (!committed->wifi.password_set) {
				(void)api_error_add_field(err, P_WIFI "/credential",
							  API_FIELD_REQUIRED);
				ok = false;
			} else if (!same_ssid || !same_security) {
				(void)api_error_add_field(err, P_WIFI "/credential",
							  API_FIELD_CONFLICTING);
				ok = false;
			}
		}
		break;

	default:
		(void)api_error_add_field(err, P_WIFI "/credential", API_FIELD_INVALID_FORMAT);
		ok = false;
		break;
	}

	return ok;
}

bool network_validate_config(const struct network_config_input *input,
			     const struct device_config *committed,
			     const struct network_status *status, struct api_error *err)
{
	const struct device_config *cfg = &input->config;
	bool ok = true;

	if (cfg->preferred_interface >= DEVICE_CONFIG_INTERFACE_COUNT) {
		(void)api_error_add_field(err, P_PREFERRED, API_FIELD_INVALID_FORMAT);
		ok = false;
	}

	if (!validate_ipv4(&cfg->ethernet.ipv4, DEVICE_CONFIG_INTERFACE_ETHERNET, err)) {
		ok = false;
	}
	if (!validate_dns(&cfg->dns, err)) {
		ok = false;
	}
	if (!validate_wifi(&cfg->wifi, &input->wifi_password, committed, err)) {
		ok = false;
	}

	/*
	 * Both interfaces off would leave nothing to manage the device with,
	 * and no API call could turn one back on.
	 */
	if (!cfg->ethernet.enabled && !cfg->wifi.enabled) {
		(void)api_error_add_field(err, P_ETH "/enabled", API_FIELD_CONFLICTING);
		(void)api_error_add_field(err, P_WIFI "/enabled", API_FIELD_CONFLICTING);
		return false;
	}

	if (ok) {
		const bool preferred_enabled =
			(cfg->preferred_interface == DEVICE_CONFIG_INTERFACE_ETHERNET)
				? cfg->ethernet.enabled
				: cfg->wifi.enabled;

		if (!preferred_enabled) {
			(void)api_error_add_field(err, P_PREFERRED, API_FIELD_CONFLICTING);
			ok = false;
		}
	}

	/*
	 * The recovery path from step 1 of section 5. At least one interface
	 * that stays enabled must have a link right now — otherwise the change
	 * is being applied over a path that does not exist, and there would be
	 * no way for anyone to tell the device to undo it.
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
