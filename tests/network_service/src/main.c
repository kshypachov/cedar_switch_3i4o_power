/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The network service's parts that run without a board.
 *
 * The slot format is exercised through device-config-store itself, over a fake
 * key-value store that behaves like settings-registry's BYTES keys: a fixed
 * blob per key, zeros for a key that was never written. That is the claim the
 * format has to meet — the store's journal and both committed copies survive
 * a reboot through it — rather than a round trip of bytes that proves only
 * that memcpy works.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <device_config_store/device_config_store.h>

#include "boot_streak.h"
#include "net_adapter_map.h"
#include "netcfg_store.h"

/* -- a key-value store like settings-registry's BYTES keys --------------------- */

struct kv_entry {
	char key[32];
	uint8_t blob[NETCFG_RECORD_BYTES];
	bool written;
};

static struct kv_entry entries[4];
static unsigned int sets;
static int fail_set;
static size_t last_set_len;

static struct kv_entry *find(const char *key, bool create)
{
	for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
		if (entries[i].written && strcmp(entries[i].key, key) == 0) {
			return &entries[i];
		}
	}
	if (!create) {
		return NULL;
	}
	for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
		if (!entries[i].written) {
			strncpy(entries[i].key, key, sizeof(entries[i].key) - 1);
			entries[i].written = true;
			return &entries[i];
		}
	}
	return NULL;
}

static int kv_get(const char *key, uint8_t *buf, size_t cap)
{
	const struct kv_entry *e = find(key, false);

	zassert_equal(cap, NETCFG_RECORD_BYTES, "the registry reads the whole blob");
	if (e == NULL) {
		memset(buf, 0, cap);
	} else {
		memcpy(buf, e->blob, cap);
	}
	return 0;
}

static int kv_set(const char *key, const uint8_t *buf, size_t len)
{
	struct kv_entry *e;

	sets++;
	last_set_len = len;
	if (fail_set != 0) {
		return fail_set;
	}
	zassert_equal(len, NETCFG_RECORD_BYTES, "a BYTES key takes exactly max_len");
	e = find(key, true);
	zassert_not_null(e);
	memcpy(e->blob, buf, len);
	return 0;
}

static const struct netcfg_kv kv = {.get = kv_get, .set = kv_set};
static struct device_config_backend backend;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(entries, 0, sizeof(entries));
	sets = 0;
	fail_set = 0;
	netcfg_store_bind(&kv, &backend);
}

ZTEST_SUITE(network_service, NULL, NULL, before, NULL, NULL);

/* -- the store's slots ----------------------------------------------------------- */

static const uint8_t password[] = "correct-horse-battery";

static void commit_wifi_profile(const char *id, uint8_t last_octet)
{
	struct device_config_update update;

	memset(&update, 0, sizeof(update));
	device_config_defaults(&update.config);
	update.config.ethernet.ipv4.mode = DEVICE_CONFIG_IPV4_STATIC;
	update.config.ethernet.ipv4.prefix_length = 24;
	update.config.ethernet.ipv4.address[0] = 192;
	update.config.ethernet.ipv4.address[1] = 168;
	update.config.ethernet.ipv4.address[2] = 88;
	update.config.ethernet.ipv4.address[3] = last_octet;
	update.config.wifi.enabled = 1;
	update.config.wifi.security = DEVICE_CONFIG_WIFI_WPA2_PSK;
	update.config.wifi.ssid_len = 5;
	memcpy(update.config.wifi.ssid, "cedar", 5);
	update.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].action = DEVICE_CONFIG_SECRET_REPLACE;
	update.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].value = password;
	update.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].len = sizeof(password) - 1;

	zassert_ok(device_config_pending_begin(&update, device_config_revision(), id));
	zassert_ok(device_config_pending_commit(id));
}

ZTEST(network_service, test_a_blank_registry_is_a_factory_device)
{
	struct device_config_recovery_report report;
	struct device_config cfg;

	zassert_ok(device_config_init(&backend, &report));
	zassert_equal(report.result, DEVICE_CONFIG_RECOVERY_DEFAULTS,
		      "keys never written read as zeros, which is empty");
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_true(cfg.ethernet.enabled);
	zassert_equal(cfg.ethernet.ipv4.mode, DEVICE_CONFIG_IPV4_DHCP);
}

ZTEST(network_service, test_commits_survive_a_reboot_through_the_slots)
{
	struct device_config_recovery_report report;
	struct device_config cfg;
	uint8_t secret[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t len = 0;

	zassert_ok(device_config_init(&backend, NULL));
	commit_wifi_profile("first", 50);
	commit_wifi_profile("second", 51);

	/* Reboot: RAM is rebuilt from what the keys hold. */
	zassert_ok(device_config_init(&backend, &report));
	zassert_equal(report.result, DEVICE_CONFIG_RECOVERY_CLEAN);
	zassert_equal(report.revision, 2);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 51, "the newer committed copy wins");
	zassert_ok(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					    DEVICE_CONFIG_SECRET_WIFI_PASSWORD, secret,
					    sizeof(secret), &len));
	zassert_equal(len, sizeof(password) - 1);
	zassert_mem_equal(secret, password, len);
}

ZTEST(network_service, test_an_unconfirmed_change_is_rolled_back_at_boot)
{
	struct device_config_update update;
	struct device_config_recovery_report report;

	zassert_ok(device_config_init(&backend, NULL));
	memset(&update, 0, sizeof(update));
	device_config_defaults(&update.config);
	update.config.ethernet.enabled = 0;
	update.config.wifi.enabled = 1;
	zassert_ok(device_config_pending_begin(&update, 0, "txn_00000007"));

	/* Power lost while awaiting confirmation. */
	zassert_ok(device_config_init(&backend, &report));
	zassert_equal(report.result, DEVICE_CONFIG_RECOVERY_ROLLED_BACK);
	zassert_str_equal(report.transaction_id, "txn_00000007");
	zassert_equal(report.revision, 0);
}

ZTEST(network_service, test_a_slot_holds_its_length_and_zeros)
{
	const uint8_t record[] = {0xAB, 0xCD, 0xEF};
	uint8_t out[8];

	zassert_ok(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_PENDING, record, sizeof(record)));
	zassert_equal(last_set_len, NETCFG_RECORD_BYTES);

	const struct kv_entry *e = find("net/config_pending", false);

	zassert_not_null(e, "the pending slot's key");
	zassert_equal(e->blob[0], 3, "length, little endian");
	zassert_equal(e->blob[1], 0);
	zassert_mem_equal(&e->blob[2], record, sizeof(record));
	for (size_t i = 2 + sizeof(record); i < NETCFG_RECORD_BYTES; i++) {
		zassert_equal(e->blob[i], 0, "padded with zeros at %zu", i);
	}

	zassert_equal(backend.read(backend.ctx, DEVICE_CONFIG_SLOT_PENDING, out, sizeof(out)), 3);
	zassert_mem_equal(out, record, sizeof(record));

	zassert_ok(backend.erase(backend.ctx, DEVICE_CONFIG_SLOT_PENDING));
	zassert_equal(backend.read(backend.ctx, DEVICE_CONFIG_SLOT_PENDING, out, sizeof(out)),
		      -ENOENT, "an erased slot is empty");
}

ZTEST(network_service, test_the_slots_have_distinct_keys)
{
	const uint8_t a = 1;
	const uint8_t b = 2;
	uint8_t out;

	zassert_ok(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_COMMITTED_A, &a, 1));
	zassert_ok(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_COMMITTED_B, &b, 1));
	zassert_equal(backend.read(backend.ctx, DEVICE_CONFIG_SLOT_COMMITTED_A, &out, 1), 1);
	zassert_equal(out, 1);
	zassert_equal(backend.read(backend.ctx, DEVICE_CONFIG_SLOT_COMMITTED_B, &out, 1), 1);
	zassert_equal(out, 2);
	zassert_str_equal(netcfg_slot_keys[DEVICE_CONFIG_SLOT_COMMITTED_A], "net/config_a");
	zassert_str_equal(netcfg_slot_keys[DEVICE_CONFIG_SLOT_COMMITTED_B], "net/config_b");
}

ZTEST(network_service, test_a_damaged_length_is_a_read_error)
{
	uint8_t out[16];
	struct kv_entry *e;
	const uint8_t record[] = {7};

	zassert_ok(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_COMMITTED_A, record, 1));
	e = find("net/config_a", false);

	e->blob[0] = 0xFF;
	e->blob[1] = 0x01; /* 511: past what a slot holds */
	zassert_equal(backend.read(backend.ctx, DEVICE_CONFIG_SLOT_COMMITTED_A, out, sizeof(out)),
		      -EIO);

	e->blob[0] = 17;
	e->blob[1] = 0; /* fits the slot, not the caller */
	zassert_equal(backend.read(backend.ctx, DEVICE_CONFIG_SLOT_COMMITTED_A, out, sizeof(out)),
		      -EIO);
}

/*
 * A stored length is judged against the slot as well as the caller's buffer:
 * 510 bytes are all a slot holds after its two length bytes, and a bigger
 * length would read past the blob. Found by mutation: the only damaged length
 * tested was also too big for the caller, so the slot bound was never needed.
 */
ZTEST(network_service, test_a_length_past_the_slot_is_a_read_error_for_any_buffer)
{
	static uint8_t out[2 * NETCFG_RECORD_BYTES];
	struct kv_entry *e;
	const uint8_t record[] = {7};

	zassert_ok(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_COMMITTED_A, record, 1));
	e = find("net/config_a", false);
	e->blob[0] = (NETCFG_RECORD_BYTES - 1) & 0xFF;
	e->blob[1] = (NETCFG_RECORD_BYTES - 1) >> 8;
	zassert_equal(backend.read(backend.ctx, DEVICE_CONFIG_SLOT_COMMITTED_A, out, sizeof(out)),
		      -EIO, "511 does not fit a slot, however big the buffer");
}

/*
 * A record is never empty: a zero length is how an erased slot reads, so
 * writing one would turn a write into an erase nobody asked for. Found by
 * mutation: dropping the check broke nothing.
 */
ZTEST(network_service, test_an_empty_record_is_not_written)
{
	const uint8_t record[] = {1};

	zassert_equal(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_PENDING, record, 0), -EINVAL);
	zassert_equal(sets, 0, "nothing reached the registry");
}

/*
 * A slot outside the three is refused before it indexes the key table. Found by
 * mutation: the bounds checks could go without any test noticing.
 */
ZTEST(network_service, test_a_slot_out_of_range_is_refused)
{
	const uint8_t record[] = {1};
	uint8_t out[4];

	zassert_equal(backend.read(backend.ctx, DEVICE_CONFIG_SLOT_COUNT, out, sizeof(out)),
		      -EINVAL);
	zassert_equal(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_COUNT, record, 1), -EINVAL);
	zassert_equal(backend.erase(backend.ctx, DEVICE_CONFIG_SLOT_COUNT), -EINVAL);
	zassert_equal(sets, 0);
}

ZTEST(network_service, test_a_record_too_big_or_a_failed_write_is_reported)
{
	static const uint8_t big[NETCFG_RECORD_BYTES];

	zassert_equal(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_PENDING, big, sizeof(big)),
		      -ENOSPC, "two bytes of every slot are its length");
	zassert_equal(sets, 0);
	zassert_ok(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_PENDING, big, sizeof(big) - 2));

	fail_set = -EIO;
	zassert_equal(backend.write(backend.ctx, DEVICE_CONFIG_SLOT_PENDING, big, 4), -EIO);
	zassert_equal(backend.erase(backend.ctx, DEVICE_CONFIG_SLOT_PENDING), -EIO);
}

/* -- the five-power-cycle count --------------------------------------------------- */

ZTEST(network_service, test_the_fifth_short_boot_in_a_row_restores)
{
	uint32_t stored = 0;
	uint32_t next;

	for (uint32_t boot = 1; boot < BOOT_STREAK_THRESHOLD; boot++) {
		zassert_false(boot_streak_step(stored, BOOT_STREAK_THRESHOLD, &next), "boot %u", boot);
		zassert_equal(next, boot);
		stored = next;
	}
	zassert_true(boot_streak_step(stored, BOOT_STREAK_THRESHOLD, &next));
	zassert_equal(next, BOOT_STREAK_THRESHOLD,
		      "stored as reached, so a power loss during the restore restores again");
	zassert_true(boot_streak_step(next, BOOT_STREAK_THRESHOLD, &next));

	zassert_true(boot_streak_step(UINT32_MAX, BOOT_STREAK_THRESHOLD, &next));
	zassert_equal(next, UINT32_MAX, "saturates");

	zassert_equal(BOOT_STREAK_THRESHOLD, 5, "the owner's five power cycles");
	zassert_equal(BOOT_STREAK_WINDOW_SECONDS, 30, "the owner's thirty seconds");
}

/* -- the adapter's translations ---------------------------------------------------- */

ZTEST(network_service, test_wifi_security_maps_to_the_contract)
{
	const struct {
		enum wifi_security_type type;
		enum network_ap_security expected;
	} table[] = {
		{WIFI_SECURITY_TYPE_NONE, NETWORK_AP_OPEN},
		{WIFI_SECURITY_TYPE_PSK, NETWORK_AP_WPA2_PSK},
		{WIFI_SECURITY_TYPE_PSK_SHA256, NETWORK_AP_WPA2_PSK},
		{WIFI_SECURITY_TYPE_FT_PSK, NETWORK_AP_WPA2_PSK},
		{WIFI_SECURITY_TYPE_SAE, NETWORK_AP_WPA3_SAE},
		{WIFI_SECURITY_TYPE_SAE_H2E, NETWORK_AP_WPA3_SAE},
		{WIFI_SECURITY_TYPE_SAE_AUTO, NETWORK_AP_WPA3_SAE},
		{WIFI_SECURITY_TYPE_FT_SAE, NETWORK_AP_WPA3_SAE},
		{WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL, NETWORK_AP_WPA2_WPA3_TRANSITION},
		{WIFI_SECURITY_TYPE_EAP, NETWORK_AP_ENTERPRISE},
		{WIFI_SECURITY_TYPE_EAP_PEAP_MSCHAPV2, NETWORK_AP_ENTERPRISE},
		/* Every PEAP inner method: found by mutation, two had no row. */
		{WIFI_SECURITY_TYPE_EAP_PEAP_GTC, NETWORK_AP_ENTERPRISE},
		{WIFI_SECURITY_TYPE_EAP_PEAP_TLS, NETWORK_AP_ENTERPRISE},
		{WIFI_SECURITY_TYPE_EAP_TTLS_MSCHAPV2, NETWORK_AP_ENTERPRISE},
		{WIFI_SECURITY_TYPE_WEP, NETWORK_AP_UNKNOWN},
		{WIFI_SECURITY_TYPE_WPA_PSK, NETWORK_AP_UNKNOWN},
		{WIFI_SECURITY_TYPE_WAPI, NETWORK_AP_UNKNOWN},
		{WIFI_SECURITY_TYPE_UNKNOWN, NETWORK_AP_UNKNOWN},
	};

	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		zassert_equal(net_map_wifi_security(table[i].type), table[i].expected,
			      "row %zu (type %d)", i, table[i].type);
	}
	/* What the contract makes selectable follows: WPA1 and WEP are not. */
	zassert_false(network_ap_security_connectable(net_map_wifi_security(WIFI_SECURITY_TYPE_WEP)));
}

ZTEST(network_service, test_prefix_lengths_and_netmasks)
{
	const struct {
		uint8_t mask[4];
		uint8_t prefix;
	} table[] = {
		{{0, 0, 0, 0}, 0},        {{128, 0, 0, 0}, 1},       {{255, 0, 0, 0}, 8},
		{{255, 255, 255, 0}, 24}, {{255, 255, 255, 252}, 30}, {{255, 255, 255, 255}, 32},
	};
	uint8_t mask[4];

	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		zassert_equal(net_map_prefix_length(table[i].mask), table[i].prefix, "row %zu", i);
		net_map_netmask(table[i].prefix, mask);
		zassert_mem_equal(mask, table[i].mask, 4, "row %zu", i);
	}
	/* A mask that is not contiguous counts its leading ones. */
	zassert_equal(net_map_prefix_length((const uint8_t[]){255, 0, 255, 0}), 8);
	net_map_netmask(40, mask);
	zassert_mem_equal(mask, ((const uint8_t[]){255, 255, 255, 255}), 4, "clamped");
}
