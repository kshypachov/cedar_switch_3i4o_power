/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API v1: network status, the committed configuration, the apply transaction
 * and Wi-Fi scans.
 *
 * Contract: "Сеть" in api-contract.md. The rules live in network-manager; this
 * file turns a request body into a proposal and the module's snapshots into
 * the schemas, and wakes the worker that does the interface work once the
 * answer is on its way. Nothing here touches an interface, so no handler waits
 * for a radio on the HTTP server's one thread.
 *
 * Decisions the contract leaves to the device:
 *
 * - Replays. apply, the discard of a staged candidate and a scan create jobs,
 *   and job-manager deduplicates them under the scoped key like every other
 *   job. Staging (201 with the transaction), confirm and the rollback of an
 *   applied change (202 with the apply job) create none, so their replays are
 *   kept here, under the same scoped key and body hash: a retry after a lost
 *   response gets the same resource rather than busy or invalid_state, and a
 *   key reused for a different body is 409 idempotency_conflict.
 * - A transaction a reboot discarded is 410 boot_changed; any other unknown id
 *   is 404.
 * - reconnect_urls are the static IPv4 addresses the candidate names and
 *   nothing else: a DHCP address is unknown until the lease arrives, and the
 *   device has no host name it can promise.
 * - An SSID is displayed as UTF-8 with each invalid sequence replaced by one
 *   U+FFFD, the way the mock decodes it, and a NUL byte replaced likewise;
 *   ssid_base64 carries the exact bytes.
 * - Only the latest scan keeps its results: an older scan's id is 410
 *   resource_expired.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/util.h>

#include <device_config_store/device_config_store.h>
#include <network_manager/network_manager.h>

#include "v1_internal.h"

LOG_MODULE_REGISTER(web_api_v1_network, LOG_LEVEL_INF);

#define CONFIG_URL      WEB_API_BASE_PATH "/network/config"
#define TRANSACTION_URL WEB_API_BASE_PATH "/network/transactions/"
#define SCAN_URL        WEB_API_BASE_PATH "/network/wifi/scans/"

#define MEMBER_SIZE(type, member) sizeof(((type *)0)->member)

/* -- the service behind the bindings --------------------------------------- */

static const struct web_api_v1_network *hooks;

void web_api_v1_set_network(const struct web_api_v1_network *network)
{
	hooks = network;
}

static void kick_worker(void)
{
	if (hooks != NULL && hooks->kick != NULL) {
		hooks->kick();
	}
}

uint32_t v1_wifi_security_modes(void)
{
	uint32_t modes = (hooks != NULL && hooks->wifi_security_modes != NULL)
				 ? hooks->wifi_security_modes()
				 : 0U;

	return (modes != 0U) ? modes : BIT_MASK(DEVICE_CONFIG_WIFI_SECURITY_COUNT);
}

/* -- request schemas --------------------------------------------------------- */

static bool is_ipv4(const char *value)
{
	uint8_t bytes[4];

	return api_parse_ipv4(value, bytes) == 0;
}

static bool is_ip(const char *value)
{
	uint8_t bytes[16];

	return api_parse_ipv4(value, bytes) == 0 || api_parse_ipv6(value, bytes) == 0;
}

static const char *const ipv4_modes[] = {"dhcp", "static", NULL};
static const char *const dns_modes[] = {"automatic", "manual", NULL};
static const char *const interface_names[] = {"ethernet", "wifi", NULL};
static const char *const security_names[] = {"open", "wpa2_psk", "wpa3_sae", NULL};
static const char *const credential_actions[] = {"keep", "replace", "clear", NULL};

#define IPV4_ADDRESS(member)                                                                       \
	{                                                                                          \
		.name = #member,                                                                   \
		.type = WEB_JSON_STRING,                                                           \
		.flags = WEB_JSON_REQUIRED | WEB_JSON_NULLABLE,                                    \
		.offset = offsetof(struct v1_ipv4_body, member),                                   \
		.null_offset = offsetof(struct v1_ipv4_body, member##_null),                       \
		.size = MEMBER_SIZE(struct v1_ipv4_body, member),                                  \
		.format = is_ipv4,                                                                 \
	}

static const struct web_json_field ipv4_fields[] = {
	{
		.name = "mode",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_ipv4_body, mode),
		.size = MEMBER_SIZE(struct v1_ipv4_body, mode),
		.enum_values = ipv4_modes,
	},
	IPV4_ADDRESS(address),
	{
		.name = "prefix_length",
		.type = WEB_JSON_INT,
		.flags = WEB_JSON_REQUIRED | WEB_JSON_NULLABLE,
		.offset = offsetof(struct v1_ipv4_body, prefix_length),
		.null_offset = offsetof(struct v1_ipv4_body, prefix_length_null),
		.min = 1,
		.max = 30,
	},
	IPV4_ADDRESS(gateway),
};

static const struct web_json_object ipv4_schema = {ipv4_fields, ARRAY_SIZE(ipv4_fields)};

static const struct web_json_field ethernet_fields[] = {
	{
		.name = "enabled",
		.type = WEB_JSON_BOOL,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_ethernet_body, enabled),
	},
	{
		.name = "ipv4",
		.type = WEB_JSON_OBJECT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_ethernet_body, ipv4),
		.object = &ipv4_schema,
	},
};

static const struct web_json_object ethernet_schema = {ethernet_fields,
						       ARRAY_SIZE(ethernet_fields)};

/*
 * CredentialChange is a oneOf of {action: keep|clear} and {action: replace,
 * value}. The descriptor takes the union of both; which branch a decoded body
 * belongs to is checked by the handler.
 */
static const struct web_json_field credential_fields[] = {
	{
		.name = "action",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_credential_body, action),
		.size = MEMBER_SIZE(struct v1_credential_body, action),
		.enum_values = credential_actions,
	},
	{
		.name = "value",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_PRESENT,
		.offset = offsetof(struct v1_credential_body, value),
		.present_offset = offsetof(struct v1_credential_body, value_present),
		.size = MEMBER_SIZE(struct v1_credential_body, value),
		.min_len = 1,
		.max_len = 64,
	},
};

static const struct web_json_object credential_schema = {credential_fields,
							 ARRAY_SIZE(credential_fields)};

static const struct web_json_field wifi_fields[] = {
	{
		.name = "enabled",
		.type = WEB_JSON_BOOL,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_wifi_body, enabled),
	},
	{
		.name = "ssid_base64",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_wifi_body, ssid_base64),
		.size = MEMBER_SIZE(struct v1_wifi_body, ssid_base64),
		.max_len = 44,
	},
	{
		.name = "security",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_wifi_body, security),
		.size = MEMBER_SIZE(struct v1_wifi_body, security),
		.enum_values = security_names,
	},
	{
		.name = "hidden",
		.type = WEB_JSON_BOOL,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_wifi_body, hidden),
	},
	{
		.name = "ipv4",
		.type = WEB_JSON_OBJECT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_wifi_body, ipv4),
		.object = &ipv4_schema,
	},
	{
		.name = "credential",
		.type = WEB_JSON_OBJECT,
		.flags = WEB_JSON_REQUIRED | WEB_JSON_ONEOF,
		.offset = offsetof(struct v1_wifi_body, credential),
		.object = &credential_schema,
	},
};

static const struct web_json_object wifi_schema = {wifi_fields, ARRAY_SIZE(wifi_fields)};

static const struct web_json_field interfaces_fields[] = {
	{
		.name = "ethernet",
		.type = WEB_JSON_OBJECT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_interfaces_body, ethernet),
		.object = &ethernet_schema,
	},
	{
		.name = "wifi",
		.type = WEB_JSON_OBJECT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_interfaces_body, wifi),
		.object = &wifi_schema,
	},
};

static const struct web_json_object interfaces_schema = {interfaces_fields,
							 ARRAY_SIZE(interfaces_fields)};

/* A server is a oneOf of an IPv4 and an IPv6 address. */
static const struct web_json_field dns_server_field = {
	.type = WEB_JSON_STRING,
	.flags = WEB_JSON_ONEOF,
	.offset = 0,
	.size = V1_IP_TEXT_MAX,
	.format = is_ip,
};

static const struct web_json_field dns_fields[] = {
	{
		.name = "mode",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_dns_body, mode),
		.size = MEMBER_SIZE(struct v1_dns_body, mode),
		.enum_values = dns_modes,
	},
	{
		.name = "servers",
		.type = WEB_JSON_ARRAY,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_dns_body, servers),
		.items = &dns_server_field,
		.item_size = V1_IP_TEXT_MAX,
		.count_offset = offsetof(struct v1_dns_body, server_count),
		.min_len = 0,
		.max_len = DEVICE_CONFIG_DNS_MAX_SERVERS,
	},
};

static const struct web_json_object dns_schema = {dns_fields, ARRAY_SIZE(dns_fields)};

static const struct web_json_field config_fields[] = {
	{
		.name = "preferred_interface",
		.type = WEB_JSON_STRING,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_network_config_body, preferred_interface),
		.size = MEMBER_SIZE(struct v1_network_config_body, preferred_interface),
		.enum_values = interface_names,
	},
	{
		.name = "dns",
		.type = WEB_JSON_OBJECT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_network_config_body, dns),
		.object = &dns_schema,
	},
	{
		.name = "interfaces",
		.type = WEB_JSON_OBJECT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_network_config_body, interfaces),
		.object = &interfaces_schema,
	},
};

static const struct web_json_object config_schema = {config_fields, ARRAY_SIZE(config_fields)};

static const struct web_json_field transaction_fields[] = {
	{
		.name = "base_revision",
		.type = WEB_JSON_INT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_network_transaction_body, base_revision),
		.min = 0,
		.max = UINT32_MAX,
	},
	{
		.name = "config",
		.type = WEB_JSON_OBJECT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_network_transaction_body, config),
		.object = &config_schema,
	},
};

const struct web_json_object v1_network_transaction_schema = {transaction_fields,
							       ARRAY_SIZE(transaction_fields)};

static const struct web_json_field apply_fields[] = {
	{
		.name = "confirmation_timeout_seconds",
		.type = WEB_JSON_INT,
		.flags = WEB_JSON_REQUIRED,
		.offset = offsetof(struct v1_apply_body, confirmation_timeout_seconds),
		.min = CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MIN_SECONDS,
		.max = CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MAX_SECONDS,
	},
};

const struct web_json_object v1_apply_schema = {apply_fields, ARRAY_SIZE(apply_fields)};

/* Empty: `{}` and nothing else. */
const struct web_json_object v1_empty_schema = {NULL, 0};

/* -- replays of requests that create no job ----------------------------------- */

#define REPLAY_SLOTS  8
#define REPLAY_TTL_MS (15 * 60 * 1000)

struct replay {
	bool used;
	char key[WEB_API_SCOPED_KEY_MAX_LEN + 1];
	uint32_t hash;
	/* The transaction staged, or the job answered with. */
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	int64_t at_ms;
};

/* Only the HTTP server's thread reaches this table. */
static struct replay replays[REPLAY_SLOTS];

enum replay_result {
	REPLAY_NONE = 0,
	REPLAY_FOUND,
	REPLAY_CONFLICT,
};

static enum replay_result replay_find(const struct web_api_call *call, char *id, size_t cap)
{
	const int64_t now = k_uptime_get();

	for (size_t i = 0; i < ARRAY_SIZE(replays); i++) {
		struct replay *r = &replays[i];

		if (!r->used) {
			continue;
		}
		if (now - r->at_ms > REPLAY_TTL_MS) {
			r->used = false;
			continue;
		}
		if (strcmp(r->key, call->scoped_key) == 0) {
			if (r->hash != call->request_hash) {
				return REPLAY_CONFLICT;
			}
			strncpy(id, r->id, cap - 1);
			id[cap - 1] = '\0';
			return REPLAY_FOUND;
		}
	}
	return REPLAY_NONE;
}

static void replay_store(const struct web_api_call *call, const char *id)
{
	struct replay *slot = &replays[0];

	for (size_t i = 0; i < ARRAY_SIZE(replays); i++) {
		if (!replays[i].used) {
			slot = &replays[i];
			break;
		}
		/* Full: the oldest record gives way. */
		if (replays[i].at_ms < slot->at_ms) {
			slot = &replays[i];
		}
	}
	slot->used = true;
	strncpy(slot->key, call->scoped_key, sizeof(slot->key) - 1);
	slot->key[sizeof(slot->key) - 1] = '\0';
	slot->hash = call->request_hash;
	strncpy(slot->id, id, sizeof(slot->id) - 1);
	slot->id[sizeof(slot->id) - 1] = '\0';
	slot->at_ms = k_uptime_get();
}

/* True when a replay or a conflict has been answered. */
static bool answered_by_replay(struct web_api_call *call, char *id, size_t cap)
{
	switch (replay_find(call, id, cap)) {
	case REPLAY_FOUND:
		return false;
	case REPLAY_CONFLICT:
		web_api_reject(call, API_ERR_IDEMPOTENCY_CONFLICT,
			       "This Idempotency-Key was used with a different request");
		return true;
	default:
		id[0] = '\0';
		return false;
	}
}

/* -- text ------------------------------------------------------------------------ */

static void ipv4_text(const uint8_t *addr, char *out, size_t cap)
{
	(void)snprintf(out, cap, "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
}

/* RFC 5952: lowercase, no leading zeros, the first longest run of zero groups as "::". */
static void ipv6_text(const uint8_t *addr, char *out, size_t cap)
{
	uint16_t groups[8];
	int best = -1;
	int best_len = 0;
	size_t pos = 0;

	for (int i = 0; i < 8; i++) {
		groups[i] = (uint16_t)((addr[2 * i] << 8) | addr[2 * i + 1]);
	}
	for (int i = 0; i < 8;) {
		int j = i;

		while (j < 8 && groups[j] == 0U) {
			j++;
		}
		if (j - i >= 2 && j - i > best_len) {
			best = i;
			best_len = j - i;
		}
		i = (j > i) ? j : i + 1;
	}

	out[0] = '\0';
	for (int i = 0; i < 8 && pos < cap; i++) {
		if (i == best) {
			pos += (size_t)snprintf(out + pos, cap - pos, "::");
			i += best_len - 1;
			continue;
		}
		if (i > 0 && !(best >= 0 && i == best + best_len)) {
			pos += (size_t)snprintf(out + pos, cap - pos, ":");
		}
		pos += (size_t)snprintf(out + pos, cap - pos, "%x", groups[i]);
	}
}

static void addr_text(const struct device_config_addr *addr, char *out, size_t cap)
{
	if (addr->family == DEVICE_CONFIG_AF_INET6) {
		ipv6_text(addr->bytes, out, cap);
	} else {
		ipv4_text(addr->bytes, out, cap);
	}
}

static void mac_text(const uint8_t *mac, char *out, size_t cap)
{
	(void)snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
		       mac[4], mac[5]);
}

/* How long a UTF-8 sequence starting with @p lead is, and the range its second byte must be in. */
static size_t utf8_length(uint8_t lead, uint8_t *lo, uint8_t *hi)
{
	*lo = 0x80;
	*hi = 0xBF;
	if (lead >= 0xC2 && lead <= 0xDF) {
		return 2;
	}
	if (lead == 0xE0) {
		*lo = 0xA0;
		return 3;
	}
	if (lead == 0xED) {
		*hi = 0x9F; /* no surrogates */
		return 3;
	}
	if (lead >= 0xE1 && lead <= 0xEF) {
		return 3;
	}
	if (lead == 0xF0) {
		*lo = 0x90;
		return 4;
	}
	if (lead >= 0xF1 && lead <= 0xF3) {
		return 4;
	}
	if (lead == 0xF4) {
		*hi = 0x8F;
		return 4;
	}
	return 0;
}

/*
 * An SSID for display: valid UTF-8 kept, and each maximal invalid subpart
 * replaced by one U+FFFD, which is what Python's decode(..., "replace") — and
 * so the mock — produces. @p cap must hold three bytes per input byte.
 */
static void ssid_text(const uint8_t *ssid, size_t len, char *out, size_t cap)
{
	size_t o = 0;

	for (size_t i = 0; i < len && o + 4U <= cap;) {
		const uint8_t c = ssid[i];
		uint8_t lo;
		uint8_t hi;
		size_t need;
		size_t ok = 0;

		if (c != 0U && c < 0x80U) {
			out[o++] = (char)c;
			i++;
			continue;
		}
		need = (c == 0U) ? 0U : utf8_length(c, &lo, &hi);
		if (need > 0U) {
			ok = 1;
			while (ok < need && i + ok < len) {
				const uint8_t b = ssid[i + ok];
				const uint8_t min = (ok == 1U) ? lo : 0x80;
				const uint8_t max = (ok == 1U) ? hi : 0xBF;

				if (b < min || b > max) {
					break;
				}
				ok++;
			}
		}
		if (need > 0U && ok == need) {
			memcpy(&out[o], &ssid[i], need);
			o += need;
			i += need;
		} else {
			out[o++] = (char)0xEF;
			out[o++] = (char)0xBF;
			out[o++] = (char)0xBD;
			i += (ok > 0U) ? ok : 1U;
		}
	}
	out[o] = '\0';
}

static void ssid_base64(const uint8_t *ssid, size_t len, char *out, size_t cap)
{
	size_t written = 0;

	if (base64_encode((uint8_t *)out, cap, &written, ssid, len) != 0) {
		written = 0;
	}
	out[MIN(written, cap - 1)] = '\0';
}

/* Strict base64, as Python's b64decode(validate=True): the alphabet, padding
 * only at the end, a length that is a multiple of four. */
static bool decode_ssid(const char *text, uint8_t *out, size_t cap, size_t *len)
{
	const size_t n = strlen(text);
	size_t padding = 0;

	if (n % 4U != 0U) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		const char c = text[i];

		if (c == '=') {
			padding++;
			continue;
		}
		if (padding > 0U ||
		    !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		      c == '+' || c == '/')) {
			return false;
		}
	}
	if (padding > 2U) {
		return false;
	}
	return base64_decode(out, cap, len, (const uint8_t *)text, n) == 0;
}

/* -- responses -------------------------------------------------------------------- */

static void write_error_detail(struct web_api_call *call, const char *code, const char *message,
			       bool retryable)
{
	struct web_json_writer *w = web_api_json(call);

	web_json_object_begin(w);
	web_json_key(w, "code");
	web_json_string(w, code);
	web_json_key(w, "message");
	web_json_string(w, message);
	web_json_key(w, "request_id");
	web_json_string(w, call->ctx->rsp.request_id);
	web_json_key(w, "retryable");
	web_json_bool(w, retryable);
	web_json_object_end(w);
}

static void write_ipv4_config(struct web_json_writer *w, const struct device_config_ipv4 *ipv4)
{
	char text[V1_IP_TEXT_MAX];
	const bool is_static = ipv4->mode == DEVICE_CONFIG_IPV4_STATIC;

	web_json_object_begin(w);
	web_json_key(w, "mode");
	web_json_string(w, device_config_ipv4_mode_str(ipv4->mode));
	web_json_key(w, "address");
	if (is_static) {
		ipv4_text(ipv4->address, text, sizeof(text));
		web_json_string(w, text);
	} else {
		web_json_null(w);
	}
	web_json_key(w, "prefix_length");
	if (is_static) {
		web_json_int(w, ipv4->prefix_length);
	} else {
		web_json_null(w);
	}
	web_json_key(w, "gateway");
	if (is_static && ipv4->has_gateway) {
		ipv4_text(ipv4->gateway, text, sizeof(text));
		web_json_string(w, text);
	} else {
		web_json_null(w);
	}
	web_json_object_end(w);
}

/* NetworkConfigOutput: the input without the credential, plus password_set. */
static void write_config_output(struct web_json_writer *w, const struct device_config *cfg)
{
	char text[V1_IP_TEXT_MAX];
	char b64[48];

	web_json_object_begin(w);
	web_json_key(w, "preferred_interface");
	web_json_string(w, device_config_interface_str(cfg->preferred_interface));
	web_json_key(w, "dns");
	web_json_object_begin(w);
	web_json_key(w, "mode");
	web_json_string(w, device_config_dns_mode_str(cfg->dns.mode));
	web_json_key(w, "servers");
	web_json_array_begin(w);
	for (uint8_t i = 0; i < MIN(cfg->dns.server_count, (uint8_t)DEVICE_CONFIG_DNS_MAX_SERVERS);
	     i++) {
		addr_text(&cfg->dns.servers[i], text, sizeof(text));
		web_json_string(w, text);
	}
	web_json_array_end(w);
	web_json_object_end(w);

	web_json_key(w, "interfaces");
	web_json_object_begin(w);
	web_json_key(w, "ethernet");
	web_json_object_begin(w);
	web_json_key(w, "enabled");
	web_json_bool(w, cfg->ethernet.enabled);
	web_json_key(w, "ipv4");
	write_ipv4_config(w, &cfg->ethernet.ipv4);
	web_json_object_end(w);

	web_json_key(w, "wifi");
	web_json_object_begin(w);
	web_json_key(w, "enabled");
	web_json_bool(w, cfg->wifi.enabled);
	web_json_key(w, "ssid_base64");
	ssid_base64(cfg->wifi.ssid, MIN(cfg->wifi.ssid_len, (uint8_t)DEVICE_CONFIG_SSID_MAX_LEN),
		    b64, sizeof(b64));
	web_json_string(w, b64);
	web_json_key(w, "security");
	web_json_string(w, device_config_wifi_security_str(cfg->wifi.security));
	web_json_key(w, "hidden");
	web_json_bool(w, cfg->wifi.hidden);
	web_json_key(w, "ipv4");
	write_ipv4_config(w, &cfg->wifi.ipv4);
	web_json_key(w, "password_set");
	web_json_bool(w, cfg->wifi.password_set);
	web_json_object_end(w);
	web_json_object_end(w);
	web_json_object_end(w);
}

static const char *transaction_error_message(const struct network_transaction *t)
{
	switch (t->error_code) {
	case API_ERR_RESOURCE_EXPIRED:
		return (t->state == NETWORK_TXN_EXPIRED)
			       ? "The candidate configuration expired before it was applied"
			       : "Confirmation timed out; the previous configuration was restored";
	case API_ERR_STALE_REVISION:
		return "The configuration changed before this one could be recorded";
	case API_ERR_INTERNAL_ERROR:
		return "The change could not be applied; the previous configuration was restored";
	default:
		return "The network change failed";
	}
}

static void write_transaction(struct web_api_call *call, const struct network_transaction *t)
{
	struct web_json_writer *w = web_api_json(call);
	char url[sizeof("http://255.255.255.255/")];

	web_json_object_begin(w);
	web_json_key(w, "id");
	web_json_string(w, t->id);
	web_json_key(w, "boot_id");
	web_json_string(w, v1_identity()->boot_id);
	web_json_key(w, "base_revision");
	web_json_int(w, t->base_revision);
	web_json_key(w, "state");
	web_json_string(w, network_transaction_state_str(t->state));
	web_json_key(w, "candidate");
	write_config_output(w, &t->candidate);
	web_json_key(w, "remaining_seconds");
	if (t->remaining_seconds >= 0) {
		web_json_int(w, t->remaining_seconds);
	} else {
		web_json_null(w);
	}

	web_json_key(w, "reconnect_urls");
	web_json_array_begin(w);
	if (t->state == NETWORK_TXN_APPLYING || t->state == NETWORK_TXN_AWAITING_CONFIRMATION) {
		const struct {
			bool enabled;
			const struct device_config_ipv4 *ipv4;
		} ifaces[] = {
			{t->candidate.ethernet.enabled, &t->candidate.ethernet.ipv4},
			{t->candidate.wifi.enabled, &t->candidate.wifi.ipv4},
		};

		for (size_t i = 0; i < ARRAY_SIZE(ifaces); i++) {
			const struct device_config_ipv4 *ipv4 = ifaces[i].ipv4;

			if (ifaces[i].enabled && ipv4->mode == DEVICE_CONFIG_IPV4_STATIC) {
				(void)snprintf(url, sizeof(url), "http://%u.%u.%u.%u/",
					       ipv4->address[0], ipv4->address[1],
					       ipv4->address[2], ipv4->address[3]);
				web_json_string(w, url);
			}
		}
	}
	web_json_array_end(w);

	web_json_key(w, "job_id");
	web_json_string_or_null(w, t->has_job ? t->job_id : NULL);
	web_json_key(w, "error");
	if (t->has_error) {
		write_error_detail(call, api_error_str(t->error_code),
				   transaction_error_message(t),
				   api_error_is_retryable(t->error_code));
	} else {
		web_json_null(w);
	}
	web_json_object_end(w);
}

static void write_addresses(struct web_json_writer *w, const struct network_iface_status *st)
{
	char text[V1_IP_TEXT_MAX];

	web_json_array_begin(w);
	for (uint8_t i = 0; i < MIN(st->addr_count, (uint8_t)NETWORK_IFACE_MAX_ADDRS); i++) {
		const struct network_addr *a = &st->addrs[i];
		const bool v6 = a->family == DEVICE_CONFIG_AF_INET6;

		if (v6) {
			ipv6_text(a->bytes, text, sizeof(text));
		} else {
			ipv4_text(a->bytes, text, sizeof(text));
		}
		web_json_object_begin(w);
		web_json_key(w, "family");
		web_json_string(w, v6 ? "ipv6" : "ipv4");
		web_json_key(w, "address");
		web_json_string(w, text);
		web_json_key(w, "prefix_length");
		web_json_int(w, a->prefix_length);
		web_json_key(w, "source");
		web_json_string(w, network_addr_source_str(a->source));
		web_json_object_end(w);
	}
	web_json_array_end(w);
}

static void write_interface(struct web_api_call *call, enum device_config_interface id,
			    const struct network_iface_status *st)
{
	struct web_json_writer *w = web_api_json(call);
	const bool wifi = id == DEVICE_CONFIG_INTERFACE_WIFI;
	char text[3 * DEVICE_CONFIG_SSID_MAX_LEN + 1];

	web_json_object_begin(w);
	web_json_key(w, "id");
	web_json_string(w, device_config_interface_str(id));
	web_json_key(w, "enabled");
	web_json_bool(w, st->enabled);
	web_json_key(w, "link_up");
	web_json_bool(w, st->link_up);
	web_json_key(w, "state");
	web_json_string(w, network_iface_state_str(st->state));
	web_json_key(w, "mac_address");
	mac_text(st->mac, text, sizeof(text));
	web_json_string(w, text);
	web_json_key(w, "addresses");
	write_addresses(w, st);
	web_json_key(w, "ssid");
	if (wifi && st->wifi_associated) {
		ssid_text(st->ssid, MIN(st->ssid_len, (uint8_t)DEVICE_CONFIG_SSID_MAX_LEN), text,
			  sizeof(text));
		web_json_string(w, text);
	} else {
		web_json_null(w);
	}
	web_json_key(w, "rssi_dbm");
	if (wifi && st->wifi_associated && st->rssi_valid) {
		web_json_int(w, CLAMP(st->rssi, -127, 0));
	} else {
		web_json_null(w);
	}
	web_json_key(w, "error");
	if (!st->present) {
		/*
		 * Carried even while the interface is disabled: that is when the
		 * screen has to explain why enabling it is refused.
		 */
		if (wifi) {
			write_error_detail(call, "capability_unavailable",
					   "The Wi-Fi coprocessor does not respond",
					   api_error_is_retryable(API_ERR_CAPABILITY_UNAVAILABLE));
		} else {
			write_error_detail(call, "service_not_ready",
					   "The Ethernet controller does not respond",
					   api_error_is_retryable(API_ERR_SERVICE_NOT_READY));
		}
	} else if (st->state == NETWORK_IFACE_FAILED) {
		write_error_detail(call, "service_not_ready",
				   "Could not join the network; check the SSID, security and password",
				   api_error_is_retryable(API_ERR_SERVICE_NOT_READY));
	} else {
		web_json_null(w);
	}
	web_json_object_end(w);
}

/* -- request to proposal --------------------------------------------------------- */

static void fill_ipv4(const struct v1_ipv4_body *body, struct device_config_ipv4 *out,
		      bool *address_given)
{
	memset(out, 0, sizeof(*out));
	out->mode = (strcmp(body->mode, "static") == 0) ? DEVICE_CONFIG_IPV4_STATIC
							: DEVICE_CONFIG_IPV4_DHCP;
	*address_given = !body->address_null;
	if (!body->address_null) {
		(void)api_parse_ipv4(body->address, out->address);
	}
	if (!body->prefix_length_null) {
		out->prefix_length = (uint8_t)body->prefix_length;
	}
	if (!body->gateway_null) {
		out->has_gateway = 1;
		(void)api_parse_ipv4(body->gateway, out->gateway);
	}
}

static uint8_t index_of(const char *const *names, const char *value)
{
	for (uint8_t i = 0; names[i] != NULL; i++) {
		if (strcmp(names[i], value) == 0) {
			return i;
		}
	}
	return 0;
}

/*
 * The decoded body as a proposal. The enums were checked by the decoder, so
 * index_of() always finds its value; the order of each names list is the
 * order of the device_config enum it maps to.
 */
static void fill_input(const struct v1_network_config_body *body, struct network_config_input *in)
{
	const struct v1_wifi_body *wifi = &body->interfaces.wifi;
	uint8_t ssid[DEVICE_CONFIG_SSID_MAX_LEN + 2];
	size_t ssid_len = 0;

	memset(in, 0, sizeof(*in));
	in->config.preferred_interface = index_of(interface_names, body->preferred_interface);

	in->config.dns.mode = index_of(dns_modes, body->dns.mode);
	in->config.dns.server_count = (uint8_t)MIN(body->dns.server_count,
						   (size_t)DEVICE_CONFIG_DNS_MAX_SERVERS);
	for (uint8_t i = 0; i < in->config.dns.server_count; i++) {
		struct device_config_addr *addr = &in->config.dns.servers[i];

		if (api_parse_ipv4(body->dns.servers[i], addr->bytes) == 0) {
			addr->family = DEVICE_CONFIG_AF_INET;
		} else if (api_parse_ipv6(body->dns.servers[i], addr->bytes) == 0) {
			addr->family = DEVICE_CONFIG_AF_INET6;
		}
	}

	in->config.ethernet.enabled = body->interfaces.ethernet.enabled;
	fill_ipv4(&body->interfaces.ethernet.ipv4, &in->config.ethernet.ipv4,
		  &in->ethernet_address_given);

	in->config.wifi.enabled = wifi->enabled;
	in->config.wifi.security = index_of(security_names, wifi->security);
	in->config.wifi.hidden = wifi->hidden;
	fill_ipv4(&wifi->ipv4, &in->config.wifi.ipv4, &in->wifi_address_given);
	if (decode_ssid(wifi->ssid_base64, ssid, sizeof(ssid), &ssid_len)) {
		/* 44 characters decode to 33 bytes: one more than an SSID holds,
		 * kept in the length so the rule can refuse it. */
		in->config.wifi.ssid_len = (uint8_t)ssid_len;
		memcpy(in->config.wifi.ssid, ssid, MIN(ssid_len, sizeof(in->config.wifi.ssid)));
	} else {
		in->ssid_invalid = true;
	}

	in->wifi_password.action = index_of(credential_actions, wifi->credential.action) == 1U
					   ? DEVICE_CONFIG_SECRET_REPLACE
				   : index_of(credential_actions, wifi->credential.action) == 2U
					   ? DEVICE_CONFIG_SECRET_CLEAR
					   : DEVICE_CONFIG_SECRET_KEEP;
	if (in->wifi_password.action == DEVICE_CONFIG_SECRET_REPLACE) {
		in->wifi_password.value = (const uint8_t *)wifi->credential.value;
		in->wifi_password.len = strlen(wifi->credential.value);
	}
}

static void wipe(void *buf, size_t len)
{
	volatile uint8_t *p = buf;

	while (len-- > 0U) {
		*p++ = 0U;
	}
}

/* -- handlers ----------------------------------------------------------------------- */

void v1_get_network_status(struct web_api_call *call)
{
	/* The HTTP server's one thread is the only caller, and this is not small. */
	static struct network_status status;
	struct web_json_writer *w;
	char text[V1_IP_TEXT_MAX];

	if (network_get_status(&status) != 0) {
		web_api_reject(call, API_ERR_SERVICE_NOT_READY, "The network service is not ready");
		return;
	}

	w = web_api_json(call);
	web_json_object_begin(w);
	web_json_key(w, "interfaces");
	web_json_array_begin(w);
	write_interface(call, DEVICE_CONFIG_INTERFACE_ETHERNET, &status.ethernet);
	write_interface(call, DEVICE_CONFIG_INTERFACE_WIFI, &status.wifi);
	web_json_array_end(w);
	web_json_key(w, "default_interface");
	web_json_string_or_null(w, status.has_active ? device_config_interface_str(status.active)
						     : NULL);
	web_json_key(w, "dns_servers");
	web_json_array_begin(w);
	for (uint8_t i = 0; i < MIN(status.dns_count, (uint8_t)DEVICE_CONFIG_DNS_MAX_SERVERS); i++) {
		addr_text(&status.dns[i], text, sizeof(text));
		web_json_string(w, text);
	}
	web_json_array_end(w);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

void v1_get_network_config(struct web_api_call *call)
{
	struct device_config cfg;
	struct network_transaction txn;
	struct web_json_writer *w;
	const bool pending = network_transaction_current(&txn) == 0 &&
			     !network_transaction_state_is_terminal(txn.state);

	if (device_config_get(DEVICE_CONFIG_COMMITTED, &cfg) != 0) {
		web_api_reject(call, API_ERR_SERVICE_NOT_READY,
			       "The configuration store is not ready");
		return;
	}

	w = web_api_json(call);
	web_json_object_begin(w);
	web_json_key(w, "revision");
	web_json_int(w, device_config_revision());
	web_json_key(w, "config");
	write_config_output(w, &cfg);
	web_json_key(w, "pending_transaction_id");
	web_json_string_or_null(w, pending ? txn.id : NULL);
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}

static void reply_transaction(struct web_api_call *call, const struct network_transaction *txn,
			      uint16_t status)
{
	char location[sizeof(TRANSACTION_URL) + NETWORK_TXN_ID_MAX_LEN];

	if (status == 201) {
		(void)snprintf(location, sizeof(location), TRANSACTION_URL "%s", txn->id);
		web_api_set_location(call, location);
	}
	write_transaction(call, txn);
	web_api_reply_json(call, status);
}

void v1_stage_network_config(struct web_api_call *call)
{
	const struct v1_network_transaction_body *body = call->body;
	/* Holds a password until the end of this call; wiped below. */
	static struct network_config_input input;
	struct network_transaction txn;
	char id[NETWORK_TXN_ID_MAX_LEN + 1];
	int rc;

	if (answered_by_replay(call, id, sizeof(id))) {
		return;
	}
	if (id[0] != '\0') {
		if (network_transaction_get(id, &txn) == 0) {
			reply_transaction(call, &txn, 201);
		} else {
			web_api_reject(call, API_ERR_NOT_FOUND,
				       "The transaction this request created no longer exists");
		}
		return;
	}

	/*
	 * The branch of CredentialChange: a replace carries a value, keep and
	 * clear do not. The decoder takes the union of both branches, so this is
	 * the part of the schema it could not check, and it is refused the way
	 * the schema would refuse it — one conflicting entry on the object.
	 */
	const struct v1_credential_body *cred = &body->config.interfaces.wifi.credential;

	if ((strcmp(cred->action, "replace") == 0) != cred->value_present) {
		(void)api_error_init(web_api_error(call), API_ERR_VALIDATION_FAILED,
				     "The request body is not acceptable", NULL);
		(void)api_error_add_field(web_api_error(call), "/config/interfaces/wifi/credential",
					  API_FIELD_CONFLICTING);
		web_api_reject_error(call);
		return;
	}

	fill_input(&body->config, &input);
	rc = network_stage_config(&input, (uint32_t)body->base_revision, web_api_error(call), &txn);
	wipe(&input, sizeof(input));
	if (rc != 0) {
		web_api_reject_error(call);
		return;
	}

	replay_store(call, txn.id);
	reply_transaction(call, &txn, 201);
}

void v1_get_network_transaction(struct web_api_call *call)
{
	struct network_transaction txn;

	if (network_transaction_get(call->params[0], &txn) == 0) {
		reply_transaction(call, &txn, 200);
	} else if (network_manager_lost_in_reboot(call->params[0])) {
		web_api_reject(call, API_ERR_BOOT_CHANGED,
			       "The device restarted; the previous configuration is in force");
	} else {
		web_api_reject(call, API_ERR_NOT_FOUND, "No such network transaction");
	}
}

void v1_apply_network_transaction(struct web_api_call *call)
{
	const struct v1_apply_body *body = call->body;
	char job_id[JOB_ID_MAX_LEN + 1];

	if (network_apply(call->params[0], (uint16_t)body->confirmation_timeout_seconds,
			  call->scoped_key, call->request_hash, web_api_error(call), job_id) != 0) {
		web_api_reject_error(call);
		return;
	}
	kick_worker();
	web_api_reply_accepted(call, job_id, CONFIG_URL);
}

void v1_confirm_network_transaction(struct web_api_call *call)
{
	char job_id[NETWORK_TXN_ID_MAX_LEN + 1];

	if (answered_by_replay(call, job_id, sizeof(job_id))) {
		return;
	}
	if (job_id[0] == '\0') {
		char accepted[JOB_ID_MAX_LEN + 1];

		if (network_confirm(call->params[0], web_api_error(call), accepted) != 0) {
			web_api_reject_error(call);
			return;
		}
		replay_store(call, accepted);
		kick_worker();
		strncpy(job_id, accepted, sizeof(job_id) - 1);
	}
	web_api_reply_accepted(call, job_id, CONFIG_URL);
}

void v1_rollback_network_transaction(struct web_api_call *call)
{
	char job_id[NETWORK_TXN_ID_MAX_LEN + 1];

	if (answered_by_replay(call, job_id, sizeof(job_id))) {
		return;
	}
	if (job_id[0] == '\0') {
		char accepted[JOB_ID_MAX_LEN + 1];

		if (network_rollback(call->params[0], call->scoped_key, call->request_hash,
				     web_api_error(call), accepted) != 0) {
			web_api_reject_error(call);
			return;
		}
		replay_store(call, accepted);
		kick_worker();
		strncpy(job_id, accepted, sizeof(job_id) - 1);
	}
	web_api_reply_accepted(call, job_id, CONFIG_URL);
}

void v1_scan_wifi(struct web_api_call *call)
{
	char job_id[JOB_ID_MAX_LEN + 1];
	char url[sizeof(SCAN_URL) + JOB_ID_MAX_LEN];

	if (network_scan_begin(call->scoped_key, call->request_hash, web_api_error(call),
			       job_id) != 0) {
		web_api_reject_error(call);
		return;
	}
	kick_worker();
	(void)snprintf(url, sizeof(url), SCAN_URL "%s", job_id);
	web_api_reply_accepted(call, job_id, url);
}

static void write_access_point(struct web_json_writer *w, const struct network_access_point *ap)
{
	char text[3 * DEVICE_CONFIG_SSID_MAX_LEN + 1];
	const size_t len = MIN(ap->ssid_len, (uint8_t)DEVICE_CONFIG_SSID_MAX_LEN);

	web_json_object_begin(w);
	web_json_key(w, "ssid");
	ssid_text(ap->ssid, len, text, sizeof(text));
	web_json_string(w, text);
	web_json_key(w, "ssid_base64");
	ssid_base64(ap->ssid, len, text, sizeof(text));
	web_json_string(w, text);
	web_json_key(w, "bssid");
	mac_text(ap->bssid, text, sizeof(text));
	web_json_string(w, text);
	web_json_key(w, "channel");
	web_json_int(w, CLAMP(ap->channel, 1, 233));
	web_json_key(w, "rssi_dbm");
	web_json_int(w, CLAMP(ap->rssi, -127, 0));
	web_json_key(w, "security");
	web_json_string(w, network_ap_security_str(ap->security));
	web_json_key(w, "connect_supported");
	web_json_bool(w, ap->connect_supported);
	web_json_object_end(w);
}

void v1_get_wifi_scan(struct web_api_call *call)
{
	/* 64 records: kilobytes, on the HTTP server's one thread. */
	static struct network_scan_results results;
	struct job_snapshot job;
	struct web_json_writer *w;
	int rc;

	if (job_get(call->params[0], &job) != 0 || job.kind != JOB_KIND_WIFI_SCAN) {
		web_api_reject(call, API_ERR_NOT_FOUND, "No such Wi-Fi scan");
		return;
	}
	rc = network_scan_results_get(job.id, &results);
	if (rc == -ENOENT) {
		web_api_reject(call, API_ERR_RESOURCE_EXPIRED, "A newer scan replaced these results");
		return;
	}

	w = web_api_json(call);
	web_json_object_begin(w);
	web_json_key(w, "job_id");
	web_json_string(w, job.id);
	web_json_key(w, "state");
	web_json_string(w, job_state_str(job.state));
	web_json_key(w, "items");
	web_json_array_begin(w);
	if (rc == 0) {
		for (uint8_t i = 0; i < results.count; i++) {
			write_access_point(w, &results.items[i]);
		}
	}
	web_json_array_end(w);
	web_json_key(w, "truncated");
	web_json_bool(w, rc == 0 && results.truncated);
	web_json_key(w, "error");
	if (job.has_error) {
		write_error_detail(call, job.error.code, "The scan failed", job.error.retryable);
	} else {
		web_json_null(w);
	}
	web_json_object_end(w);
	web_api_reply_json(call, 200);
}
