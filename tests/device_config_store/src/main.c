/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device configuration store unit tests.
 *
 * Everything runs on native_sim. The store's storage backend is injected, so
 * the cases that matter most here — a write torn by power loss, a reboot
 * between the commit and the cleanup, a slot gone bad — are produced on demand
 * rather than staged on a board with a bench supply.
 *
 * "Reboot" throughout means: drop the store's RAM state and call
 * device_config_init() again against the same storage. That is exactly what a
 * real reboot does to this module, since it keeps nothing else.
 */

#include <string.h>

#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <device_config_store/device_config_store.h>

#include "fake_storage.h"

static struct fake_storage storage;
static struct device_config_backend backend;

static const uint8_t wifi_password[] = "correct-horse-battery";
static const uint8_t admin_verifier[] = {0xA5, 0x5A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

static void case_before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_storage_init(&storage);
	fake_storage_bind(&storage, &backend);
	zassert_ok(device_config_init(&backend, NULL));
}

ZTEST_SUITE(device_config_store, NULL, NULL, case_before, NULL, NULL);

/* Drop RAM state and reload from the same storage. */
static enum device_config_recovery reboot(void)
{
	struct device_config_recovery_report report;

	zassert_ok(device_config_init(&backend, &report));

	return report.result;
}

static enum device_config_recovery reboot_report(struct device_config_recovery_report *report)
{
	zassert_ok(device_config_init(&backend, report));

	return report->result;
}

/* A configuration distinguishable from the defaults, for checking what stuck. */
static struct device_config_update make_update(uint8_t last_octet)
{
	struct device_config_update u;

	memset(&u, 0, sizeof(u));
	device_config_defaults(&u.config);

	u.config.preferred_interface = DEVICE_CONFIG_INTERFACE_WIFI;
	u.config.ethernet.ipv4.mode = DEVICE_CONFIG_IPV4_STATIC;
	u.config.ethernet.ipv4.prefix_length = 24;
	u.config.ethernet.ipv4.address[0] = 192;
	u.config.ethernet.ipv4.address[1] = 168;
	u.config.ethernet.ipv4.address[2] = 88;
	u.config.ethernet.ipv4.address[3] = last_octet;
	u.config.ethernet.ipv4.has_gateway = 1;
	u.config.ethernet.ipv4.gateway[0] = 192;
	u.config.ethernet.ipv4.gateway[1] = 168;
	u.config.ethernet.ipv4.gateway[2] = 88;
	u.config.ethernet.ipv4.gateway[3] = 1;

	u.config.wifi.enabled = 1;
	u.config.wifi.security = DEVICE_CONFIG_WIFI_WPA3_SAE;
	u.config.wifi.ssid_len = 5;
	memcpy(u.config.wifi.ssid, "cedar", 5);

	u.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	u.config.dns.server_count = 1;
	u.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET;
	u.config.dns.servers[0].bytes[0] = 192;
	u.config.dns.servers[0].bytes[1] = 168;
	u.config.dns.servers[0].bytes[2] = 88;
	u.config.dns.servers[0].bytes[3] = 1;

	return u;
}

static void set_wifi_password(struct device_config_update *u, const uint8_t *value, size_t len)
{
	u->secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].action = DEVICE_CONFIG_SECRET_REPLACE;
	u->secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].value = value;
	u->secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].len = len;
}

/* Stage and commit in one step, for tests that need history rather than detail. */
static void commit_generation(uint8_t last_octet, const char *txn)
{
	struct device_config_update u = make_update(last_octet);

	zassert_ok(device_config_pending_begin(&u, device_config_revision(), txn));
	zassert_ok(device_config_pending_commit(txn));
}

/* --- defaults and first boot ------------------------------------------- */

ZTEST(device_config_store, test_blank_storage_yields_defaults)
{
	struct device_config_recovery_report report;
	struct device_config cfg;

	fake_storage_init(&storage);
	zassert_equal(reboot_report(&report), DEVICE_CONFIG_RECOVERY_DEFAULTS);
	zassert_equal(report.revision, 0);
	zassert_equal(report.transaction_id[0], '\0');

	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.preferred_interface, DEVICE_CONFIG_INTERFACE_ETHERNET);
	zassert_equal(cfg.ethernet.enabled, 1, "first boot must come up on Ethernet");
	zassert_equal(cfg.ethernet.ipv4.mode, DEVICE_CONFIG_IPV4_DHCP);
	zassert_equal(cfg.wifi.enabled, 0, "Wi-Fi with no SSID must not be enabled");
	zassert_equal(cfg.wifi.password_set, 0);
	zassert_equal(cfg.admin_password_set, 0);
}

ZTEST(device_config_store, test_defaults_do_not_touch_storage)
{
	/* Coming up on defaults must not write; a read-only failure is not a
	 * reason to start overwriting slots that might still be recoverable.
	 */
	fake_storage_init(&storage);
	(void)reboot();

	for (int s = 0; s < DEVICE_CONFIG_SLOT_COUNT; s++) {
		zassert_equal(storage.writes[s], 0, "slot %d was written on a defaults boot", s);
	}
}

ZTEST(device_config_store, test_init_rejects_incomplete_backend)
{
	struct device_config_backend broken = backend;

	broken.write = NULL;
	zassert_equal(device_config_init(&broken, NULL), -EINVAL);
	zassert_equal(device_config_init(NULL, NULL), -EINVAL);
}

/* --- the happy transaction --------------------------------------------- */

ZTEST(device_config_store, test_begin_journals_without_committing)
{
	struct device_config_update u = make_update(14);
	struct device_config cfg;
	struct device_config_pending_info info;

	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));

	/* Committed is untouched: the network has not been changed yet. */
	zassert_equal(device_config_revision(), 0);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.mode, DEVICE_CONFIG_IPV4_DHCP);

	/* Pending carries the proposal. */
	zassert_ok(device_config_get(DEVICE_CONFIG_PENDING, &cfg));
	zassert_equal(cfg.ethernet.ipv4.mode, DEVICE_CONFIG_IPV4_STATIC);
	zassert_equal(cfg.ethernet.ipv4.address[3], 14);

	zassert_ok(device_config_pending_info_get(&info));
	zassert_true(info.present);
	zassert_equal(strcmp(info.transaction_id, "txn-1"), 0);
	zassert_equal(info.base_revision, 0);
	zassert_equal(info.revision, 1);
}

ZTEST(device_config_store, test_commit_advances_the_revision)
{
	struct device_config_update u = make_update(14);
	struct device_config cfg;
	struct device_config_pending_info info;

	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_pending_commit("txn-1"));

	zassert_equal(device_config_revision(), 1);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 14);
	zassert_equal(cfg.dns.mode, DEVICE_CONFIG_DNS_MANUAL);
	zassert_equal(cfg.dns.servers[0].bytes[3], 1);

	/* The journal is gone, and with it the pending generation. */
	zassert_ok(device_config_pending_info_get(&info));
	zassert_false(info.present);
	zassert_equal(device_config_get(DEVICE_CONFIG_PENDING, &cfg), -ENOENT);
}

ZTEST(device_config_store, test_committed_configuration_survives_reboot)
{
	commit_generation(14, "txn-1");

	zassert_equal(reboot(), DEVICE_CONFIG_RECOVERY_CLEAN);

	struct device_config cfg;

	zassert_equal(device_config_revision(), 1);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 14);
}

ZTEST(device_config_store, test_rollback_restores_nothing_but_discards_the_journal)
{
	struct device_config_update u = make_update(14);

	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_pending_rollback("txn-1"));

	struct device_config_pending_info info;

	zassert_ok(device_config_pending_info_get(&info));
	zassert_false(info.present);
	zassert_equal(device_config_revision(), 0);

	/* And the slot is free for the next attempt. */
	zassert_ok(device_config_pending_begin(&u, 0, "txn-2"));
}

/* --- concurrency and staleness ----------------------------------------- */

ZTEST(device_config_store, test_second_transaction_is_refused)
{
	struct device_config_update u = make_update(14);

	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_equal(device_config_pending_begin(&u, 0, "txn-2"), -EBUSY,
		      "two journalled transactions would make rollback ambiguous");
}

ZTEST(device_config_store, test_stale_base_revision_is_refused)
{
	struct device_config_update u = make_update(14);

	commit_generation(14, "txn-1");
	zassert_equal(device_config_revision(), 1);

	zassert_equal(device_config_pending_begin(&u, 0, "txn-2"), -ESTALE,
		      "a candidate built against revision 0 must not apply over revision 1");
	zassert_ok(device_config_pending_begin(&u, 1, "txn-2"));
}

ZTEST(device_config_store, test_commit_and_rollback_check_the_transaction_id)
{
	struct device_config_update u = make_update(14);

	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));

	zassert_equal(device_config_pending_commit("txn-other"), -EINVAL,
		      "a confirmation must not commit someone else's transaction");
	zassert_equal(device_config_pending_rollback("txn-other"), -EINVAL);

	zassert_ok(device_config_pending_commit("txn-1"));
	zassert_equal(device_config_pending_commit("txn-1"), -ENOENT);
	zassert_equal(device_config_pending_rollback("txn-1"), -ENOENT);
}

ZTEST(device_config_store, test_transaction_id_is_validated)
{
	struct device_config_update u = make_update(14);

	zassert_equal(device_config_pending_begin(&u, 0, NULL), -EINVAL);
	zassert_equal(device_config_pending_begin(&u, 0, ""), -EINVAL);
	zassert_equal(device_config_pending_begin(&u, 0, "has space"), -EINVAL);
	zassert_equal(device_config_pending_begin(&u, 0, "has/slash"), -EINVAL,
		      "an id goes straight into a URL");
	zassert_equal(device_config_pending_begin(NULL, 0, "txn-1"), -EINVAL);

	/* Exactly 64 characters is the documented maximum and must be accepted. */
	char max_id[DEVICE_CONFIG_TXN_ID_MAX_LEN + 1];

	memset(max_id, 'a', DEVICE_CONFIG_TXN_ID_MAX_LEN);
	max_id[DEVICE_CONFIG_TXN_ID_MAX_LEN] = '\0';
	zassert_ok(device_config_pending_begin(&u, 0, max_id));
}

/* --- secrets ------------------------------------------------------------ */

ZTEST(device_config_store, test_secret_is_stored_and_flagged)
{
	struct device_config_update u = make_update(14);
	uint8_t buf[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t len = 0;
	struct device_config cfg;

	set_wifi_password(&u, wifi_password, sizeof(wifi_password) - 1);
	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));

	/* The pending password is readable during apply: it is the network
	 * being joined.
	 */
	zassert_ok(device_config_secret_get(DEVICE_CONFIG_PENDING,
					    DEVICE_CONFIG_SECRET_WIFI_PASSWORD, buf, sizeof(buf),
					    &len));
	zassert_equal(len, sizeof(wifi_password) - 1);
	zassert_mem_equal(buf, wifi_password, len);

	zassert_ok(device_config_get(DEVICE_CONFIG_PENDING, &cfg));
	zassert_equal(cfg.wifi.password_set, 1);

	zassert_ok(device_config_pending_commit("txn-1"));
	zassert_ok(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					    DEVICE_CONFIG_SECRET_WIFI_PASSWORD, buf, sizeof(buf),
					    &len));
	zassert_equal(len, sizeof(wifi_password) - 1);
}

/*
 * The contract's rule that an absent credential must not clear a stored one.
 * KEEP is the zero value precisely so a zero-initialised update is safe.
 */
ZTEST(device_config_store, test_keep_carries_the_secret_forward)
{
	struct device_config_update u = make_update(14);
	uint8_t buf[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t len = 0;

	set_wifi_password(&u, wifi_password, sizeof(wifi_password) - 1);
	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_pending_commit("txn-1"));

	/* A second generation that says nothing about the password. */
	struct device_config_update quiet = make_update(15);

	zassert_equal(quiet.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].action,
		      DEVICE_CONFIG_SECRET_KEEP, "zero-initialised must mean keep");
	zassert_ok(device_config_pending_begin(&quiet, 1, "txn-2"));
	zassert_ok(device_config_pending_commit("txn-2"));

	zassert_ok(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					    DEVICE_CONFIG_SECRET_WIFI_PASSWORD, buf, sizeof(buf),
					    &len));
	zassert_equal(len, sizeof(wifi_password) - 1, "password was lost by an unrelated change");
	zassert_mem_equal(buf, wifi_password, len);
}

ZTEST(device_config_store, test_keep_survives_a_rolled_back_transaction)
{
	struct device_config_update u = make_update(14);
	uint8_t buf[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t len = 0;

	set_wifi_password(&u, wifi_password, sizeof(wifi_password) - 1);
	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_pending_commit("txn-1"));

	struct device_config_update quiet = make_update(15);

	zassert_ok(device_config_pending_begin(&quiet, 1, "txn-2"));
	zassert_ok(device_config_pending_rollback("txn-2"));

	zassert_ok(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					    DEVICE_CONFIG_SECRET_WIFI_PASSWORD, buf, sizeof(buf),
					    &len));
	zassert_equal(len, sizeof(wifi_password) - 1);
}

ZTEST(device_config_store, test_clear_removes_the_secret)
{
	struct device_config_update u = make_update(14);
	uint8_t buf[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t len = 99;
	struct device_config cfg;

	set_wifi_password(&u, wifi_password, sizeof(wifi_password) - 1);
	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_pending_commit("txn-1"));

	struct device_config_update cleared = make_update(14);

	cleared.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].action = DEVICE_CONFIG_SECRET_CLEAR;
	zassert_ok(device_config_pending_begin(&cleared, 1, "txn-2"));
	zassert_ok(device_config_pending_commit("txn-2"));

	zassert_ok(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					    DEVICE_CONFIG_SECRET_WIFI_PASSWORD, buf, sizeof(buf),
					    &len));
	zassert_equal(len, 0);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.wifi.password_set, 0);
}

ZTEST(device_config_store, test_secrets_are_independent)
{
	struct device_config_update u = make_update(14);
	uint8_t buf[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t len = 0;
	struct device_config cfg;

	set_wifi_password(&u, wifi_password, sizeof(wifi_password) - 1);
	u.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].action = DEVICE_CONFIG_SECRET_REPLACE;
	u.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].value = admin_verifier;
	u.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].len = sizeof(admin_verifier);
	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_pending_commit("txn-1"));

	/* Clearing the Wi-Fi password must not touch the admin credential. */
	struct device_config_update cleared = make_update(14);

	cleared.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].action = DEVICE_CONFIG_SECRET_CLEAR;
	zassert_ok(device_config_pending_begin(&cleared, 1, "txn-2"));
	zassert_ok(device_config_pending_commit("txn-2"));

	zassert_ok(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					    DEVICE_CONFIG_SECRET_ADMIN_PASSWORD, buf, sizeof(buf),
					    &len));
	zassert_equal(len, sizeof(admin_verifier));
	zassert_mem_equal(buf, admin_verifier, len);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.admin_password_set, 1);
	zassert_equal(cfg.wifi.password_set, 0);
}

ZTEST(device_config_store, test_secret_flags_are_derived_not_trusted)
{
	struct device_config_update u = make_update(14);
	struct device_config cfg;

	/* A caller that lies about having a password must not be believed. */
	u.config.wifi.password_set = 1;
	u.config.admin_password_set = 1;
	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_get(DEVICE_CONFIG_PENDING, &cfg));
	zassert_equal(cfg.wifi.password_set, 0, "flag must follow the stored secret");
	zassert_equal(cfg.admin_password_set, 0);

	/* And one that forgets the flag must not lose a real password. */
	zassert_ok(device_config_pending_rollback("txn-1"));

	struct device_config_update honest = make_update(14);

	set_wifi_password(&honest, wifi_password, sizeof(wifi_password) - 1);
	honest.config.wifi.password_set = 0;
	zassert_ok(device_config_pending_begin(&honest, 0, "txn-2"));
	zassert_ok(device_config_get(DEVICE_CONFIG_PENDING, &cfg));
	zassert_equal(cfg.wifi.password_set, 1);
}

ZTEST(device_config_store, test_secret_accessor_rejects_bad_arguments)
{
	struct device_config_update u = make_update(14);
	uint8_t buf[4];
	size_t len = 0;

	set_wifi_password(&u, wifi_password, sizeof(wifi_password) - 1);
	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_pending_commit("txn-1"));

	zassert_equal(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					       DEVICE_CONFIG_SECRET_WIFI_PASSWORD, buf, sizeof(buf),
					       &len),
		      -ENOMEM, "a short buffer must fail, not truncate a password");
	zassert_equal(device_config_secret_get(DEVICE_CONFIG_COMMITTED, DEVICE_CONFIG_SECRET_COUNT,
					       buf, sizeof(buf), &len),
		      -EINVAL);
	zassert_equal(device_config_secret_get(DEVICE_CONFIG_PENDING,
					       DEVICE_CONFIG_SECRET_WIFI_PASSWORD, buf, sizeof(buf),
					       &len),
		      -ENOENT, "no pending generation exists");
}

ZTEST(device_config_store, test_replace_requires_a_value)
{
	struct device_config_update u = make_update(14);

	u.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].action = DEVICE_CONFIG_SECRET_REPLACE;
	u.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].value = NULL;
	u.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].len = 0;
	zassert_equal(device_config_pending_begin(&u, 0, "txn-1"), -EINVAL);

	set_wifi_password(&u, wifi_password, DEVICE_CONFIG_SECRET_MAX_LEN + 1);
	zassert_equal(device_config_pending_begin(&u, 0, "txn-1"), -EINVAL);
}

/*
 * The structural half of the secret rule: whatever an HTTP handler does with a
 * snapshot, it cannot leak a password, because the bytes are not in it.
 */
ZTEST(device_config_store, test_snapshot_contains_no_secret_material)
{
	struct device_config_update u = make_update(14);
	struct device_config cfg;

	set_wifi_password(&u, wifi_password, sizeof(wifi_password) - 1);
	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_pending_commit("txn-1"));

	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));

	const uint8_t *bytes = (const uint8_t *)&cfg;
	const size_t needle = sizeof(wifi_password) - 1;

	for (size_t i = 0; i + needle <= sizeof(cfg); i++) {
		zassert_true(memcmp(&bytes[i], wifi_password, needle) != 0,
			     "password found at offset %zu of a snapshot", i);
	}
}

/* --- power loss --------------------------------------------------------- */

/*
 * The confirmation timeout's reboot branch: an apply that reached storage and
 * changed the network, then lost power before anyone confirmed it.
 */
ZTEST(device_config_store, test_reboot_before_commit_rolls_back)
{
	struct device_config_recovery_report report;
	struct device_config cfg;

	commit_generation(14, "txn-1");

	struct device_config_update u = make_update(99);

	zassert_ok(device_config_pending_begin(&u, 1, "txn-2"));

	zassert_equal(reboot_report(&report), DEVICE_CONFIG_RECOVERY_ROLLED_BACK);
	zassert_equal(report.revision, 1);
	zassert_equal(strcmp(report.transaction_id, "txn-2"), 0,
		      "the client is still polling this id and must be told");

	zassert_equal(device_config_revision(), 1);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 14, "the old configuration must be back");

	/* And the journal is gone, so a second reboot is quiet. */
	zassert_equal(reboot(), DEVICE_CONFIG_RECOVERY_CLEAN);
}

/*
 * The other side of the same instant: power lost after the committed record
 * was durable but before the journal was erased. The commit had succeeded, so
 * it must stand.
 */
ZTEST(device_config_store, test_reboot_after_commit_keeps_the_new_configuration)
{
	struct device_config_recovery_report report;
	struct device_config cfg;

	commit_generation(14, "txn-1");

	struct device_config_update u = make_update(99);

	zassert_ok(device_config_pending_begin(&u, 1, "txn-2"));

	/* Commit, but let the journal erase fail — the same storage state
	 * power loss between the two steps would leave.
	 */
	storage.fail_erase_slot = DEVICE_CONFIG_SLOT_PENDING;
	zassert_ok(device_config_pending_commit("txn-2"),
		   "a journal that will not erase is not a failed commit");
	zassert_equal(device_config_revision(), 2);

	zassert_equal(reboot_report(&report), DEVICE_CONFIG_RECOVERY_COMMIT_COMPLETED);
	zassert_equal(report.revision, 2);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 99);

	zassert_equal(reboot(), DEVICE_CONFIG_RECOVERY_CLEAN, "the leftover was cleaned up");
}

ZTEST(device_config_store, test_torn_journal_write_leaves_nothing_in_flight)
{
	struct device_config_update u = make_update(99);

	commit_generation(14, "txn-1");

	storage.tear_write_slot = DEVICE_CONFIG_SLOT_PENDING;
	storage.tear_after = 40;
	zassert_equal(device_config_pending_begin(&u, 1, "txn-2"), -EIO,
		      "an unverifiable journal must not authorise touching the network");

	struct device_config_pending_info info;

	zassert_ok(device_config_pending_info_get(&info));
	zassert_false(info.present);

	/* Nothing survives to confuse the next boot. */
	zassert_equal(reboot(), DEVICE_CONFIG_RECOVERY_CLEAN);
	zassert_equal(device_config_revision(), 1);
}

ZTEST(device_config_store, test_failed_journal_write_is_reported_and_retryable)
{
	struct device_config_update u = make_update(99);

	storage.fail_write_slot = DEVICE_CONFIG_SLOT_PENDING;
	zassert_equal(device_config_pending_begin(&u, 0, "txn-1"), -EIO);

	/* The store is not wedged: the next attempt works. */
	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));
	zassert_ok(device_config_pending_commit("txn-1"));
	zassert_equal(device_config_revision(), 1);
}

ZTEST(device_config_store, test_torn_commit_write_keeps_the_old_generation)
{
	struct device_config cfg;

	commit_generation(14, "txn-1");

	struct device_config_update u = make_update(99);

	zassert_ok(device_config_pending_begin(&u, 1, "txn-2"));

	/* Revision 1 is in slot A, so the commit of revision 2 targets B. */
	storage.tear_write_slot = DEVICE_CONFIG_SLOT_COMMITTED_B;
	storage.tear_after = 50;
	zassert_equal(device_config_pending_commit("txn-2"), -EIO);
	zassert_equal(device_config_revision(), 1, "a failed commit must not advance anything");

	/* Reboot: B is unreadable, A still holds revision 1, and the journal
	 * describes a transaction that never committed.
	 */
	struct device_config_recovery_report report;

	zassert_equal(reboot_report(&report), DEVICE_CONFIG_RECOVERY_ROLLED_BACK);
	zassert_equal(report.revision, 1);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 14);
}

ZTEST(device_config_store, test_commit_never_overwrites_the_live_slot)
{
	/* The ping-pong is the reason a torn commit is survivable at all, so
	 * the alternation itself is asserted rather than assumed.
	 */
	commit_generation(14, "txn-1");
	zassert_true(storage.slots[DEVICE_CONFIG_SLOT_COMMITTED_A].present);
	zassert_false(storage.slots[DEVICE_CONFIG_SLOT_COMMITTED_B].present);

	commit_generation(15, "txn-2");
	zassert_true(storage.slots[DEVICE_CONFIG_SLOT_COMMITTED_B].present);

	fake_storage_reset_injection(&storage);
	commit_generation(16, "txn-3");
	zassert_equal(storage.writes[DEVICE_CONFIG_SLOT_COMMITTED_A], 1,
		      "revision 3 belongs in A, over the superseded revision 1");
	zassert_equal(storage.writes[DEVICE_CONFIG_SLOT_COMMITTED_B], 0);
}

ZTEST(device_config_store, test_corrupt_newest_slot_falls_back_to_the_older_one)
{
	struct device_config_recovery_report report;
	struct device_config cfg;

	commit_generation(14, "txn-1"); /* revision 1 -> A */
	commit_generation(99, "txn-2"); /* revision 2 -> B */

	/* Bit rot in the payload of the newest record. */
	fake_storage_corrupt_byte(&storage, DEVICE_CONFIG_SLOT_COMMITTED_B, 120);

	zassert_equal(reboot_report(&report), DEVICE_CONFIG_RECOVERY_COMMITTED_DEGRADED,
		      "running on a stale generation must not be silent");
	zassert_equal(device_config_revision(), 1);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 14);
}

ZTEST(device_config_store, test_both_slots_lost_reports_more_than_a_blank_device)
{
	struct device_config_recovery_report report;

	commit_generation(14, "txn-1");
	commit_generation(99, "txn-2");

	fake_storage_corrupt_byte(&storage, DEVICE_CONFIG_SLOT_COMMITTED_A, 120);
	fake_storage_corrupt_byte(&storage, DEVICE_CONFIG_SLOT_COMMITTED_B, 120);

	zassert_equal(reboot_report(&report), DEVICE_CONFIG_RECOVERY_COMMITTED_LOST,
		      "a device that lost its configuration is not a new device");
	zassert_equal(device_config_revision(), 0);

	/* Still reachable, which is the point of falling back rather than refusing. */
	struct device_config cfg;

	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.enabled, 1);
	zassert_equal(cfg.ethernet.ipv4.mode, DEVICE_CONFIG_IPV4_DHCP);
}

ZTEST(device_config_store, test_corrupt_journal_is_discarded)
{
	struct device_config_recovery_report report;

	commit_generation(14, "txn-1");

	struct device_config_update u = make_update(99);

	zassert_ok(device_config_pending_begin(&u, 1, "txn-2"));
	fake_storage_corrupt_byte(&storage, DEVICE_CONFIG_SLOT_PENDING, 4);

	zassert_equal(reboot_report(&report), DEVICE_CONFIG_RECOVERY_PENDING_CORRUPT);
	zassert_equal(device_config_revision(), 1);
	zassert_equal(reboot(), DEVICE_CONFIG_RECOVERY_CLEAN);
}

ZTEST(device_config_store, test_failed_rollback_keeps_the_transaction_visible)
{
	struct device_config_update u = make_update(99);
	struct device_config_pending_info info;

	zassert_ok(device_config_pending_begin(&u, 0, "txn-1"));

	storage.fail_erase_slot = DEVICE_CONFIG_SLOT_PENDING;
	zassert_equal(device_config_pending_rollback("txn-1"), -EIO);

	zassert_ok(device_config_pending_info_get(&info));
	zassert_true(info.present,
		     "claiming a rollback that never reached storage would let a reboot "
		     "resurrect the transaction");

	zassert_ok(device_config_pending_rollback("txn-1"));
	zassert_ok(device_config_pending_info_get(&info));
	zassert_false(info.present);
}

/* --- schema ------------------------------------------------------------- */

/*
 * A record from a schema this build has never seen must not be reinterpreted.
 * The version field sits at a known offset in the header, so it can be raised
 * directly on storage — which is exactly what a firmware downgrade would find.
 * The header CRC is recomputed so the record is otherwise perfectly valid and
 * the version gate is what rejects it, not the checksum.
 */
static void bump_stored_schema_version(enum device_config_slot slot, uint16_t version)
{
	struct fake_slot *s = &storage.slots[slot];
	uint32_t crc;

	memcpy(&s->data[4], &version, sizeof(version));
	/* Header CRC is the last uint32 of the 96-byte header. */
	crc = crc32_ieee(s->data, 92);
	memcpy(&s->data[92], &crc, sizeof(crc));
}

ZTEST(device_config_store, test_newer_schema_is_refused_not_guessed_at)
{
	struct device_config_recovery_report report;

	commit_generation(14, "txn-1");
	bump_stored_schema_version(DEVICE_CONFIG_SLOT_COMMITTED_A,
				   DEVICE_CONFIG_SCHEMA_VERSION + 1);

	zassert_equal(reboot_report(&report), DEVICE_CONFIG_RECOVERY_COMMITTED_LOST);
	zassert_equal(device_config_revision(), 0);
}

ZTEST(device_config_store, test_unmigratable_older_schema_is_refused)
{
	struct device_config_recovery_report report;

	commit_generation(14, "txn-1");
	/* Version 0 predates every migration this build carries. */
	bump_stored_schema_version(DEVICE_CONFIG_SLOT_COMMITTED_A, 0);

	zassert_equal(reboot_report(&report), DEVICE_CONFIG_RECOVERY_COMMITTED_LOST,
		      "an unreadable old record must not be read as the current layout");
	zassert_equal(device_config_revision(), 0);
}

/* --- wire names --------------------------------------------------------- */

/* These strings are the HTTP contract; a typo here is a broken API. */
ZTEST(device_config_store, test_wire_names_match_the_contract)
{
	zassert_equal(strcmp(device_config_wifi_security_str(DEVICE_CONFIG_WIFI_WPA3_SAE),
			     "wpa3_sae"),
		      0);
	zassert_equal(strcmp(device_config_wifi_security_str(DEVICE_CONFIG_WIFI_WPA2_PSK),
			     "wpa2_psk"),
		      0);
	zassert_equal(strcmp(device_config_wifi_security_str(DEVICE_CONFIG_WIFI_OPEN), "open"), 0);
	zassert_equal(strcmp(device_config_ipv4_mode_str(DEVICE_CONFIG_IPV4_STATIC), "static"), 0);
	zassert_equal(strcmp(device_config_ipv4_mode_str(DEVICE_CONFIG_IPV4_DHCP), "dhcp"), 0);
	zassert_equal(strcmp(device_config_dns_mode_str(DEVICE_CONFIG_DNS_AUTOMATIC), "automatic"),
		      0);
	zassert_equal(strcmp(device_config_dns_mode_str(DEVICE_CONFIG_DNS_MANUAL), "manual"), 0);
	zassert_equal(strcmp(device_config_interface_str(DEVICE_CONFIG_INTERFACE_ETHERNET),
			     "ethernet"),
		      0);
	zassert_equal(strcmp(device_config_interface_str(DEVICE_CONFIG_INTERFACE_WIFI), "wifi"), 0);

	for (int i = 0; i < DEVICE_CONFIG_WIFI_SECURITY_COUNT; i++) {
		zassert_not_null(device_config_wifi_security_str(i), "security %d has no name", i);
	}
	zassert_is_null(device_config_wifi_security_str(DEVICE_CONFIG_WIFI_SECURITY_COUNT));
	zassert_is_null(device_config_ipv4_mode_str(DEVICE_CONFIG_IPV4_MODE_COUNT));
	zassert_is_null(device_config_dns_mode_str(DEVICE_CONFIG_DNS_MODE_COUNT));
	zassert_is_null(device_config_interface_str(DEVICE_CONFIG_INTERFACE_COUNT));
}
