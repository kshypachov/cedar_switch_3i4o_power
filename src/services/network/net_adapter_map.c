/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See net_adapter_map.h.
 */

#include "net_adapter_map.h"

enum network_ap_security net_map_wifi_security(enum wifi_security_type type)
{
	switch (type) {
	case WIFI_SECURITY_TYPE_NONE:
		return NETWORK_AP_OPEN;
	case WIFI_SECURITY_TYPE_PSK:
	case WIFI_SECURITY_TYPE_PSK_SHA256:
	case WIFI_SECURITY_TYPE_FT_PSK:
		return NETWORK_AP_WPA2_PSK;
	case WIFI_SECURITY_TYPE_SAE_HNP:
	case WIFI_SECURITY_TYPE_SAE_H2E:
	case WIFI_SECURITY_TYPE_SAE_AUTO:
	case WIFI_SECURITY_TYPE_FT_SAE:
		return NETWORK_AP_WPA3_SAE;
	case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
		return NETWORK_AP_WPA2_WPA3_TRANSITION;
	case WIFI_SECURITY_TYPE_EAP:
	case WIFI_SECURITY_TYPE_EAP_PEAP_MSCHAPV2:
	case WIFI_SECURITY_TYPE_EAP_PEAP_GTC:
	case WIFI_SECURITY_TYPE_EAP_TTLS_MSCHAPV2:
	case WIFI_SECURITY_TYPE_EAP_PEAP_TLS:
		return NETWORK_AP_ENTERPRISE;
	default:
		return NETWORK_AP_UNKNOWN;
	}
}

uint8_t net_map_prefix_length(const uint8_t mask[4])
{
	uint8_t bits = 0;

	for (int i = 0; i < 4; i++) {
		for (int b = 7; b >= 0; b--) {
			if ((mask[i] & (1U << b)) == 0U) {
				return bits;
			}
			bits++;
		}
	}
	return bits;
}

void net_map_netmask(uint8_t prefix_length, uint8_t mask[4])
{
	const uint32_t value = (prefix_length == 0U)   ? 0U
			       : (prefix_length >= 32U) ? 0xFFFFFFFFU
							: (0xFFFFFFFFU << (32U - prefix_length));

	mask[0] = (uint8_t)(value >> 24);
	mask[1] = (uint8_t)(value >> 16);
	mask[2] = (uint8_t)(value >> 8);
	mask[3] = (uint8_t)value;
}
