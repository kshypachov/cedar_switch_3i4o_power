/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network manager unit tests.
 *
 * The transaction here is almost entirely about failure: a password that does
 * not work, a cable that is out, a client that never confirms. None of those
 * can be produced on demand against real hardware, so both the interfaces and
 * the clock are injected and the tests drive them directly.
 *
 * Section 12 of the development plan asks this tier for four things — the full
 * transaction through to committed and to rolled_back, IPv4/mask/DNS
 * validation, the confirmation timer, and the health policy over fake
 * interfaces. They are the four groups below.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <device_config_store/device_config_store.h>
#include <job_manager/job_manager.h>
#include <network_manager/network_manager.h>

#include "fake_iface.h"
#include "fake_storage.h"

static struct fake_storage storage;
static struct device_config_backend store_backend;
static struct fake_net net;
static struct network_iface_ops ops;

/* Injected clock. Tests move time explicitly; nothing here waits. */
static int64_t fake_now;

static int64_t fake_clock(void)
{
	return fake_now;
}

static void advance_seconds(int32_t seconds)
{
	fake_now += (int64_t)seconds * 1000;
}

static void bring_up(void)
{
	fake_net_bind(&net, &ops);
	zassert_ok(network_manager_init(&ops));
}

static void case_before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_now = 1000;
	job_manager_set_clock(fake_clock);
	network_manager_set_clock(fake_clock);

	job_manager_init();
	fake_storage_init(&storage);
	fake_storage_bind(&storage, &store_backend);
	zassert_ok(device_config_init(&store_backend, NULL));

	fake_net_init(&net);
	bring_up();
}

ZTEST_SUITE(network_manager, NULL, NULL, case_before, NULL, NULL);

/* --- helpers ------------------------------------------------------------ */

static const uint8_t wifi_password[] = "correct-horse-battery";

/* Ethernet on a static address, Wi-Fi off: valid, and different enough from
 * the defaults that applying it is visible.
 */
static struct network_config_input valid_input(void)
{
	struct network_config_input in;

	memset(&in, 0, sizeof(in));
	device_config_defaults(&in.config);

	in.config.ethernet.enabled = 1;
	in.config.ethernet.ipv4.mode = DEVICE_CONFIG_IPV4_STATIC;
	in.config.ethernet.ipv4.prefix_length = 24;
	in.config.ethernet.ipv4.address[0] = 192;
	in.config.ethernet.ipv4.address[1] = 168;
	in.config.ethernet.ipv4.address[2] = 88;
	in.config.ethernet.ipv4.address[3] = 14;
	in.config.ethernet.ipv4.has_gateway = 1;
	in.config.ethernet.ipv4.gateway[0] = 192;
	in.config.ethernet.ipv4.gateway[1] = 168;
	in.config.ethernet.ipv4.gateway[2] = 88;
	in.config.ethernet.ipv4.gateway[3] = 1;

	return in;
}

/* Wi-Fi enabled alongside Ethernet, with a password supplied. */
static struct network_config_input wifi_input(void)
{
	struct network_config_input in = valid_input();

	in.config.wifi.enabled = 1;
	in.config.wifi.security = DEVICE_CONFIG_WIFI_WPA3_SAE;
	in.config.wifi.ssid_len = 5;
	memcpy(in.config.wifi.ssid, "cedar", 5);
	in.config.wifi.ipv4.mode = DEVICE_CONFIG_IPV4_DHCP;
	in.wifi_password.action = DEVICE_CONFIG_SECRET_REPLACE;
	in.wifi_password.value = wifi_password;
	in.wifi_password.len = sizeof(wifi_password) - 1;

	return in;
}

static int stage(const struct network_config_input *in, struct api_error *err,
		 struct network_transaction *txn)
{
	return network_stage_config(in, device_config_revision(), err, txn);
}

/* Stage, apply and run the worker, leaving the transaction awaiting confirmation. */
static void apply_to_awaiting(const struct network_config_input *in,
			      struct network_transaction *txn)
{
	struct api_error err;

	zassert_ok(stage(in, &err, txn));
	zassert_ok(network_apply(txn->id, 0, &err));
	zassert_ok(network_apply_execute(txn->id));
	zassert_ok(network_transaction_get(txn->id, txn));
	zassert_equal(txn->state, NETWORK_TXN_AWAITING_CONFIRMATION);
}

/*
 * Stage something expected to be valid and immediately discard it. Only one
 * transaction exists at a time, so a case that checks a valid configuration
 * before an invalid one has to free the slot in between — otherwise the second
 * call is refused as busy and never reaches the validator at all.
 */
static void stage_and_discard(const struct network_config_input *in)
{
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(in, &err, &txn));
	zassert_ok(network_rollback(txn.id, &err));
}

/* True when the error carries a field with this path and code. */
static bool has_field(const struct api_error *err, const char *path, enum api_field_code code)
{
	for (uint8_t i = 0; i < err->field_count; i++) {
		if (strcmp(err->fields[i].path, path) == 0 && err->fields[i].code == code) {
			return true;
		}
	}

	return false;
}

/* --- validation: IPv4 and masks ----------------------------------------- */

ZTEST(network_manager, test_valid_configuration_stages)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));
	zassert_equal(txn.state, NETWORK_TXN_STAGED);
	zassert_equal(txn.base_revision, 0);
	zassert_false(txn.has_job, "a staged candidate has changed nothing yet");
	zassert_true(txn.remaining_seconds > 0, "the candidate must be on a deadline");
	zassert_equal(net.eth.configure_calls, 0, "staging must not touch the hardware");
}

ZTEST(network_manager, test_static_requires_an_address)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	memset(in.config.ethernet.ipv4.address, 0, 4);
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_equal(err.code, API_ERR_VALIDATION_FAILED);
	zassert_true(has_field(&err, "/interfaces/ethernet/ipv4/address", API_FIELD_REQUIRED),
		     "field detail missing");
}

ZTEST(network_manager, test_prefix_bounds)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.ethernet.ipv4.prefix_length = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(
		has_field(&err, "/interfaces/ethernet/ipv4/prefix_length", API_FIELD_REQUIRED));

	in.config.ethernet.ipv4.prefix_length = 31;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/ethernet/ipv4/prefix_length",
			       API_FIELD_OUT_OF_RANGE),
		     "the schema stops at /30");

	in.config.ethernet.ipv4.prefix_length = 30;
	in.config.ethernet.ipv4.address[3] = 13;
	in.config.ethernet.ipv4.gateway[3] = 14;
	zassert_ok(stage(&in, &err, NULL), "a /30 with both hosts in it is legal");
}

/*
 * The addresses a kernel accepts and then does not route. Each of these would
 * leave the operator looking at a device that took the setting and went quiet.
 */
ZTEST(network_manager, test_unusable_host_addresses_are_refused)
{
	const struct {
		uint8_t addr[4];
		const char *why;
	} bad[] = {
		{{192, 168, 88, 0}, "the subnet address"},
		{{192, 168, 88, 255}, "the broadcast address"},
		{{127, 0, 0, 1}, "loopback"},
		{{169, 254, 1, 1}, "link-local, which a failed DHCP assigns itself"},
		{{239, 255, 0, 1}, "multicast"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		struct network_config_input in = valid_input();
		struct api_error err;

		memcpy(in.config.ethernet.ipv4.address, bad[i].addr, 4);
		in.config.ethernet.ipv4.has_gateway = 0;
		zassert_equal(stage(&in, &err, NULL), -EINVAL, "accepted %s", bad[i].why);
		zassert_true(has_field(&err, "/interfaces/ethernet/ipv4/address",
				       API_FIELD_OUT_OF_RANGE),
			     "%s", bad[i].why);
	}
}

ZTEST(network_manager, test_gateway_must_be_reachable)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	/* Off-subnet: unreachable without a route that does not exist yet. */
	in.config.ethernet.ipv4.gateway[2] = 89;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/ethernet/ipv4/gateway", API_FIELD_CONFLICTING));

	/* The interface's own address is not a gateway. */
	in = valid_input();
	memcpy(in.config.ethernet.ipv4.gateway, in.config.ethernet.ipv4.address, 4);
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/ethernet/ipv4/gateway", API_FIELD_CONFLICTING));

	/* An unusable host address is no better as a gateway. */
	in = valid_input();
	in.config.ethernet.ipv4.gateway[3] = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/ethernet/ipv4/gateway", API_FIELD_OUT_OF_RANGE));
}

ZTEST(network_manager, test_gateway_is_optional)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	/* An isolated LAN with no router is a legitimate configuration. */
	in.config.ethernet.ipv4.has_gateway = 0;
	memset(in.config.ethernet.ipv4.gateway, 0, 4);
	zassert_ok(stage(&in, &err, NULL));
}

/*
 * DHCP is held to the same standard as static. Storing an address beside
 * mode=dhcp would mean the configuration no longer says what was asked for,
 * and whoever read it back could not tell which half was in use.
 */
ZTEST(network_manager, test_dhcp_rejects_static_fields)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.ethernet.ipv4.mode = DEVICE_CONFIG_IPV4_DHCP;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/ethernet/ipv4/address", API_FIELD_CONFLICTING));
	zassert_true(
		has_field(&err, "/interfaces/ethernet/ipv4/prefix_length", API_FIELD_CONFLICTING));
	zassert_true(has_field(&err, "/interfaces/ethernet/ipv4/gateway", API_FIELD_CONFLICTING));
}

/* --- validation: DNS ---------------------------------------------------- */

ZTEST(network_manager, test_dns_automatic_takes_no_servers)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.dns.mode = DEVICE_CONFIG_DNS_AUTOMATIC;
	in.config.dns.server_count = 1;
	in.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET;
	in.config.dns.servers[0].bytes[0] = 1;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/dns/servers", API_FIELD_CONFLICTING));
}

ZTEST(network_manager, test_dns_manual_needs_at_least_one_server)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL,
		      "manual with no resolver is not the same as automatic");
	zassert_true(has_field(&err, "/dns/servers", API_FIELD_REQUIRED));
}

ZTEST(network_manager, test_dns_manual_accepts_both_families)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 2;
	in.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET;
	in.config.dns.servers[0].bytes[0] = 192;
	in.config.dns.servers[0].bytes[1] = 168;
	in.config.dns.servers[0].bytes[2] = 88;
	in.config.dns.servers[0].bytes[3] = 1;
	in.config.dns.servers[1].family = DEVICE_CONFIG_AF_INET6;
	in.config.dns.servers[1].bytes[0] = 0x20;
	in.config.dns.servers[1].bytes[1] = 0x01;
	in.config.dns.servers[1].bytes[15] = 1;
	stage_and_discard(&in);

	/* Three is past the limit the plan sets for v1. */
	in.config.dns.server_count = 3;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/dns/servers", API_FIELD_TOO_LONG));
}

ZTEST(network_manager, test_dns_rejects_the_unspecified_address)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 1;
	in.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET;
	zassert_equal(stage(&in, &err, NULL), -EINVAL, "0.0.0.0 is not a resolver");
	zassert_true(has_field(&err, "/dns/servers", API_FIELD_INVALID_FORMAT));
}

/* --- validation: reachability ------------------------------------------- */

ZTEST(network_manager, test_both_interfaces_disabled_is_refused)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.ethernet.enabled = 0;
	in.config.wifi.enabled = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL,
		      "no API call could turn one back on afterwards");
	zassert_true(has_field(&err, "/interfaces/ethernet/enabled", API_FIELD_CONFLICTING));
	zassert_true(has_field(&err, "/interfaces/wifi/enabled", API_FIELD_CONFLICTING));
}

ZTEST(network_manager, test_preferred_must_be_enabled)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.preferred_interface = DEVICE_CONFIG_INTERFACE_WIFI;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/preferred_interface", API_FIELD_CONFLICTING));
}

/*
 * The recovery path from step 1 of section 5: without a live link on some
 * interface that stays enabled, the change is being applied over a path that
 * does not exist and nobody could tell the device to undo it.
 */
ZTEST(network_manager, test_a_recovery_path_is_required)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	net.eth.link_up = false;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/ethernet/enabled", API_FIELD_NOT_ALLOWED));
	zassert_true(has_field(&err, "/interfaces/wifi/enabled", API_FIELD_NOT_ALLOWED));

	/* An associated Wi-Fi is a perfectly good recovery path. */
	struct network_config_input wifi = wifi_input();

	net.wifi.link_up = true;
	zassert_ok(stage(&wifi, &err, NULL));
}

/* Link, not address: the address is exactly what is about to change, so
 * requiring the new one to work already would refuse every static change.
 */
ZTEST(network_manager, test_recovery_path_accepts_a_link_without_an_address)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	net.eth.dhcp_answers = false;
	net.eth.configured = false;
	zassert_ok(stage(&in, &err, NULL));
}

/* --- validation: Wi-Fi credentials -------------------------------------- */

ZTEST(network_manager, test_enabled_wifi_needs_an_ssid)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	in.config.wifi.ssid_len = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/wifi/ssid_base64", API_FIELD_REQUIRED));
}

ZTEST(network_manager, test_protected_network_needs_a_password)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	in.wifi_password.action = DEVICE_CONFIG_SECRET_KEEP;
	in.wifi_password.value = NULL;
	in.wifi_password.len = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL, "nothing is stored to keep");
	zassert_true(has_field(&err, "/interfaces/wifi/credential", API_FIELD_REQUIRED));
}

/*
 * The stored password belongs to the network it was entered for. Carrying it
 * to a different SSID would produce a profile that silently cannot associate,
 * with nothing in the API to show why.
 */
ZTEST(network_manager, test_keep_is_refused_when_the_network_changed)
{
	struct network_config_input in = wifi_input();
	struct network_transaction txn;
	struct api_error err;

	net.wifi.link_up = true;
	apply_to_awaiting(&in, &txn);
	zassert_ok(network_confirm(txn.id, &err));

	/* Same SSID and security: keeping the password is fine. */
	struct network_config_input again = wifi_input();

	again.wifi_password.action = DEVICE_CONFIG_SECRET_KEEP;
	again.wifi_password.value = NULL;
	again.wifi_password.len = 0;
	stage_and_discard(&again);

	/* A different SSID with the old password is not. */
	struct network_config_input moved = again;

	memcpy(moved.config.wifi.ssid, "other", 5);
	zassert_equal(stage(&moved, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/wifi/credential", API_FIELD_CONFLICTING));

	/* So is a different security mode. */
	struct network_config_input resecured = again;

	resecured.config.wifi.security = DEVICE_CONFIG_WIFI_WPA2_PSK;
	zassert_equal(stage(&resecured, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/wifi/credential", API_FIELD_CONFLICTING));
}

ZTEST(network_manager, test_open_network_credential_rules)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	/* A password offered for an open network is a contradiction. */
	in.config.wifi.security = DEVICE_CONFIG_WIFI_OPEN;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/wifi/credential", API_FIELD_CONFLICTING));

	/* Clearing it is the documented way. */
	in.wifi_password.action = DEVICE_CONFIG_SECRET_CLEAR;
	in.wifi_password.value = NULL;
	in.wifi_password.len = 0;
	net.wifi.link_up = true;
	stage_and_discard(&in);

	/* And clearing a protected network's password is not. */
	struct network_config_input protected_clear = wifi_input();

	protected_clear.wifi_password.action = DEVICE_CONFIG_SECRET_CLEAR;
	protected_clear.wifi_password.value = NULL;
	protected_clear.wifi_password.len = 0;
	zassert_equal(stage(&protected_clear, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/interfaces/wifi/credential", API_FIELD_CONFLICTING));
}

ZTEST(network_manager, test_disabled_wifi_is_not_held_to_the_rules)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	/* Settings are kept so re-enabling restores the profile, but an SSID
	 * is not demanded of an interface that is off.
	 */
	in.config.wifi.enabled = 0;
	in.config.wifi.security = DEVICE_CONFIG_WIFI_WPA3_SAE;
	in.config.wifi.ssid_len = 0;
	zassert_ok(stage(&in, &err, NULL));
}

/* --- staging concurrency ------------------------------------------------ */

ZTEST(network_manager, test_stale_base_revision_is_refused)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	apply_to_awaiting(&in, &txn);
	zassert_ok(network_confirm(txn.id, &err));
	zassert_equal(device_config_revision(), 1);

	zassert_equal(network_stage_config(&in, 0, &err, NULL), -EINVAL);
	zassert_equal(err.code, API_ERR_STALE_REVISION,
		      "a candidate built on revision 0 must not silently undo revision 1");
}

ZTEST(network_manager, test_only_one_transaction_at_a_time)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_equal(err.code, API_ERR_BUSY,
		      "replacing a transaction would strand whoever is waiting on it");

	/* Once it has finished, the slot is free again. */
	zassert_ok(network_rollback(txn.id, &err));
	zassert_ok(stage(&in, &err, NULL));
}

/* --- the transaction ---------------------------------------------------- */

ZTEST(network_manager, test_full_transaction_reaches_committed)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config cfg;

	zassert_ok(stage(&in, &err, &txn));
	zassert_equal(device_config_revision(), 0);

	zassert_ok(network_apply(txn.id, 0, &err));
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_APPLYING);
	zassert_true(txn.has_job, "an accepted change is tracked by a job");
	zassert_equal(device_config_revision(), 0, "apply must not commit");
	zassert_equal(net.eth.configure_calls, 0,
		      "the network must not change before the response is sent");

	/* The journal is durable before anything is touched. */
	struct device_config_pending_info pending;

	zassert_ok(device_config_pending_info_get(&pending));
	zassert_true(pending.present);
	zassert_equal(strcmp(pending.transaction_id, txn.id), 0);

	zassert_ok(network_apply_execute(txn.id));
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_AWAITING_CONFIRMATION);
	zassert_equal(net.eth.configure_calls, 1);
	zassert_equal(net.eth.applied.address[3], 14);
	zassert_equal(device_config_revision(), 0, "still not committed");

	zassert_ok(network_confirm(txn.id, &err));
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_COMMITTED);
	zassert_equal(device_config_revision(), 1);

	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 14);

	/* The journal is gone, and the job succeeded. */
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);

	struct job_snapshot job;

	zassert_ok(job_get(txn.job_id, &job));
	zassert_equal(job.state, JOB_STATE_SUCCEEDED);
}

ZTEST(network_manager, test_the_committed_configuration_survives_a_reboot)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config_recovery_report report;
	struct device_config cfg;

	apply_to_awaiting(&in, &txn);
	zassert_ok(network_confirm(txn.id, &err));

	zassert_ok(device_config_init(&store_backend, &report));
	zassert_equal(report.result, DEVICE_CONFIG_RECOVERY_CLEAN);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 14);
}

ZTEST(network_manager, test_rollback_from_staged_changes_nothing)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config_pending_info pending;

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_rollback(txn.id, &err));

	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_ROLLED_BACK);
	zassert_equal(net.eth.configure_calls, 0);
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);
	zassert_equal(device_config_revision(), 0);
}

ZTEST(network_manager, test_rollback_after_apply_restores_the_interfaces)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config_pending_info pending;

	apply_to_awaiting(&in, &txn);
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_STATIC);

	zassert_ok(network_rollback(txn.id, &err));
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_ROLLED_BACK);

	/* Back to the committed generation, which is still the DHCP default. */
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP);
	zassert_equal(device_config_revision(), 0);
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);

	struct job_snapshot job;

	zassert_ok(job_get(txn.job_id, &job));
	zassert_equal(job.state, JOB_STATE_CANCELLED);
}

/*
 * A change that cannot be recorded is a change that cannot be undone, so the
 * hardware must not be touched at all.
 */
ZTEST(network_manager, test_a_journal_that_cannot_be_written_stops_the_apply)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));
	storage.fail_write_slot = DEVICE_CONFIG_SLOT_PENDING;

	zassert_equal(network_apply(txn.id, 0, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INTERNAL_ERROR);

	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_FAILED);
	zassert_equal(net.eth.configure_calls, 0);
	zassert_equal(device_config_revision(), 0);
}

ZTEST(network_manager, test_an_adapter_failure_restores_the_committed_config)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config_pending_info pending;

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_apply(txn.id, 0, &err));

	net.fail_configure = -EIO;
	zassert_equal(network_apply_execute(txn.id), -EIO);

	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_FAILED);
	zassert_true(txn.has_error);

	/* Nothing is left behind: not on the hardware, not in the journal. */
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP);
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);
	zassert_equal(device_config_revision(), 0);
}

ZTEST(network_manager, test_state_transitions_are_guarded)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));

	/* Confirming something that was never applied. */
	zassert_equal(network_confirm(txn.id, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INVALID_STATE);

	zassert_ok(network_apply(txn.id, 0, &err));

	/* Applying twice. */
	zassert_equal(network_apply(txn.id, 0, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INVALID_STATE);

	/*
	 * Rolling back while the worker is between the journal and the
	 * interfaces: nobody could say whether the hardware had been touched.
	 */
	zassert_equal(network_rollback(txn.id, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_BUSY);

	zassert_ok(network_apply_execute(txn.id));
	zassert_ok(network_confirm(txn.id, &err));

	/* And nothing works once it is finished. */
	zassert_equal(network_confirm(txn.id, &err), -EINVAL);
	zassert_equal(network_rollback(txn.id, &err), -EINVAL);
}

ZTEST(network_manager, test_unknown_transaction_ids)
{
	struct api_error err;
	struct network_transaction txn;

	zassert_equal(network_transaction_get("txn_nope", &txn), -ENOENT);
	zassert_equal(network_transaction_current(&txn), -ENOENT);

	zassert_equal(network_apply("txn_nope", 0, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_NOT_FOUND);
	zassert_equal(network_confirm("txn_nope", &err), -EINVAL);
	zassert_equal(err.code, API_ERR_NOT_FOUND);
	zassert_equal(network_rollback("txn_nope", &err), -EINVAL);
	zassert_equal(err.code, API_ERR_NOT_FOUND);
}

/* --- health policy ------------------------------------------------------ */

/*
 * The rule the contract states in prose: a client reaching the confirm
 * endpoint proves its own path works and nothing else. Here the browser is on
 * the unchanged Ethernet while Wi-Fi failed to associate.
 */
ZTEST(network_manager, test_confirm_waits_for_every_enabled_interface)
{
	struct network_config_input in = wifi_input();
	struct api_error err;
	struct network_transaction txn;

	net.wifi.wifi_associates = false;
	apply_to_awaiting(&in, &txn);

	zassert_equal(network_confirm(txn.id, &err), -EAGAIN,
		      "Ethernet working says nothing about whether Wi-Fi joined");
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_AWAITING_CONFIRMATION,
		      "not healthy yet is not the same as failed");
	zassert_equal(device_config_revision(), 0);

	/* Once the radio does associate, the same call succeeds. */
	net.wifi.wifi_associates = true;
	net.wifi.link_up = true;
	zassert_ok(network_confirm(txn.id, &err));
	zassert_equal(device_config_revision(), 1);
}

ZTEST(network_manager, test_confirm_waits_for_dhcp)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	in.config.ethernet.ipv4.mode = DEVICE_CONFIG_IPV4_DHCP;
	in.config.ethernet.ipv4.prefix_length = 0;
	memset(in.config.ethernet.ipv4.address, 0, 4);
	in.config.ethernet.ipv4.has_gateway = 0;
	memset(in.config.ethernet.ipv4.gateway, 0, 4);

	net.eth.dhcp_answers = false;
	apply_to_awaiting(&in, &txn);
	zassert_equal(network_confirm(txn.id, &err), -EAGAIN, "no lease, no address");

	net.eth.dhcp_answers = true;
	zassert_ok(network_confirm(txn.id, &err));
}

/*
 * Link and address alone would let a device with no way off its own subnet
 * look healthy, so a configuration that asked for a gateway needs the route.
 */
ZTEST(network_manager, test_a_requested_gateway_must_produce_a_route)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	apply_to_awaiting(&in, &txn);
	zassert_ok(network_confirm(txn.id, &err));

	/* Without a gateway there is no route to require. */
	fake_net_init(&net);
	bring_up();
	job_manager_init();

	struct network_config_input no_gw = valid_input();

	no_gw.config.ethernet.ipv4.address[3] = 15;
	no_gw.config.ethernet.ipv4.has_gateway = 0;
	memset(no_gw.config.ethernet.ipv4.gateway, 0, 4);

	struct network_transaction second;

	zassert_ok(network_stage_config(&no_gw, device_config_revision(), &err, &second));
	zassert_ok(network_apply(second.id, 0, &err));
	zassert_ok(network_apply_execute(second.id));
	zassert_false(net.eth.has_route, "no gateway was configured");
	zassert_ok(network_confirm(second.id, &err), "and none is required");
}

/*
 * The gap this fills was found by mutation: removing the route requirement
 * from the health check broke nothing, because every fake interface that had
 * an address also had a route. Link and address alone would let a device with
 * no way off its own subnet report itself healthy and keep the change.
 */
ZTEST(network_manager, test_a_configured_gateway_without_a_route_is_not_healthy)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	net.eth.suppress_route = true;
	apply_to_awaiting(&in, &txn);
	zassert_true(net.eth.has_ipv4, "the address is there");
	zassert_false(net.eth.has_route, "but nothing routes off the subnet");

	zassert_equal(network_confirm(txn.id, &err), -EAGAIN);
	zassert_equal(device_config_revision(), 0);

	net.eth.suppress_route = false;
	zassert_ok(network_confirm(txn.id, &err));
	zassert_equal(device_config_revision(), 1);
}

ZTEST(network_manager, test_a_disabled_interface_is_not_health_checked)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	/* Wi-Fi is off and cannot associate; that must not block the commit. */
	net.wifi.wifi_associates = false;
	net.wifi.present = false;
	apply_to_awaiting(&in, &txn);
	zassert_ok(network_confirm(txn.id, &err));
	zassert_equal(net.disconnect_calls, 1, "a disabled radio is taken down");
}

/* --- the confirmation timer --------------------------------------------- */

ZTEST(network_manager, test_a_candidate_nobody_applies_expires)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));

	advance_seconds(CONFIG_NETWORK_MANAGER_CANDIDATE_TTL_SECONDS - 1);
	zassert_equal(network_manager_tick(), 0);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_STAGED);

	advance_seconds(2);
	zassert_equal(network_manager_tick(), 1);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_EXPIRED);

	/* And the slot is free for the next attempt. */
	zassert_ok(stage(&in, &err, NULL));
}

/*
 * The case this module exists for. The change went live, the client can no
 * longer reach the device, and nobody is coming to confirm it.
 */
ZTEST(network_manager, test_silence_rolls_the_change_back)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;
	struct device_config_pending_info pending;

	apply_to_awaiting(&in, &txn);
	zassert_equal(net.eth.applied.address[3], 14);

	advance_seconds(CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_SECONDS - 1);
	zassert_equal(network_manager_tick(), 0, "the deadline has not passed yet");

	advance_seconds(2);
	zassert_equal(network_manager_tick(), 1);

	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_ROLLED_BACK);
	zassert_true(txn.has_error);

	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP,
		      "the interfaces must be back on the committed configuration");
	zassert_equal(device_config_revision(), 0);
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);

	struct job_snapshot job;

	zassert_ok(job_get(txn.job_id, &job));
	zassert_equal(job.state, JOB_STATE_FAILED);
}

/* A worker that never runs must not leave the change live forever either. */
ZTEST(network_manager, test_a_worker_that_never_runs_still_times_out)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_apply(txn.id, 0, &err));

	advance_seconds(CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_SECONDS + 1);
	zassert_equal(network_manager_tick(), 1);

	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_ROLLED_BACK);
}

ZTEST(network_manager, test_remaining_seconds_counts_down)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));
	zassert_equal(txn.remaining_seconds, CONFIG_NETWORK_MANAGER_CANDIDATE_TTL_SECONDS);

	advance_seconds(60);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.remaining_seconds, CONFIG_NETWORK_MANAGER_CANDIDATE_TTL_SECONDS - 60);

	zassert_ok(network_apply(txn.id, 90, &err));
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.remaining_seconds, 90, "apply restarts the clock on its own deadline");

	/* A finished transaction has no deadline to report. */
	zassert_ok(network_apply_execute(txn.id));
	zassert_ok(network_confirm(txn.id, &err));
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.remaining_seconds, -1);
}

ZTEST(network_manager, test_confirmation_timeout_is_bounded)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));

	zassert_equal(network_apply(txn.id, 1, &err), -EINVAL,
		      "a client that only has to reconnect would run out of time");
	zassert_true(has_field(&err, "/confirmation_timeout_seconds", API_FIELD_OUT_OF_RANGE));

	zassert_equal(network_apply(txn.id, 3600, &err), -EINVAL,
		      "longer than anyone waits before power-cycling the device");

	zassert_ok(network_apply(txn.id, CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MIN_SECONDS,
				 &err));
}

/* --- secrets ------------------------------------------------------------ */

ZTEST(network_manager, test_the_password_reaches_the_radio_and_nowhere_else)
{
	struct network_config_input in = wifi_input();
	struct network_transaction txn;

	net.wifi.link_up = true;
	apply_to_awaiting(&in, &txn);

	zassert_equal(net.password_len, sizeof(wifi_password) - 1);
	zassert_mem_equal(net.password, wifi_password, net.password_len);

	/* And it is not in anything the API serialises. */
	const uint8_t *bytes = (const uint8_t *)&txn.candidate;
	const size_t needle = sizeof(wifi_password) - 1;

	for (size_t i = 0; i + needle <= sizeof(txn.candidate); i++) {
		zassert_true(memcmp(&bytes[i], wifi_password, needle) != 0,
			     "password found in a transaction snapshot at offset %zu", i);
	}
	zassert_equal(txn.candidate.wifi.password_set, 1);
}

/*
 * A network change must not be able to touch the credential that guards the
 * API. The input type has no field for it, and this checks the store agrees.
 */
ZTEST(network_manager, test_a_network_change_cannot_alter_the_admin_password)
{
	const uint8_t verifier[] = {0xA5, 0x5A, 0x01, 0x02};
	struct device_config_update seed;
	uint8_t buf[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t len = 0;

	memset(&seed, 0, sizeof(seed));
	device_config_defaults(&seed.config);
	seed.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].action = DEVICE_CONFIG_SECRET_REPLACE;
	seed.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].value = verifier;
	seed.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].len = sizeof(verifier);
	zassert_ok(device_config_pending_begin(&seed, 0, "seed-admin"));
	zassert_ok(device_config_pending_commit("seed-admin"));

	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	apply_to_awaiting(&in, &txn);
	zassert_ok(network_confirm(txn.id, &err));

	zassert_ok(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					    DEVICE_CONFIG_SECRET_ADMIN_PASSWORD, buf,
					    sizeof(buf), &len));
	zassert_equal(len, sizeof(verifier), "the admin credential was lost");
	zassert_mem_equal(buf, verifier, len);
}

/* --- status ------------------------------------------------------------- */

ZTEST(network_manager, test_status_reports_the_observed_route)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct network_status status;

	apply_to_awaiting(&in, &txn);
	zassert_ok(network_confirm(txn.id, &err));

	zassert_ok(network_get_status(&status));
	zassert_true(status.ethernet.link_up);
	zassert_true(status.ethernet.has_ipv4);
	zassert_equal(status.ethernet.ipv4[3], 14);
	zassert_true(status.has_active);
	zassert_equal(status.active, DEVICE_CONFIG_INTERFACE_ETHERNET);

	/* Pulling the cable is visible immediately: status is read through, not cached. */
	net.eth.link_up = false;
	zassert_ok(network_get_status(&status));
	zassert_false(status.ethernet.link_up);
	zassert_false(status.ethernet.has_ipv4);
}

ZTEST(network_manager, test_status_reports_manual_resolvers)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct network_status status;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 1;
	in.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET;
	in.config.dns.servers[0].bytes[0] = 192;
	in.config.dns.servers[0].bytes[1] = 168;
	in.config.dns.servers[0].bytes[2] = 88;
	in.config.dns.servers[0].bytes[3] = 1;

	apply_to_awaiting(&in, &txn);
	zassert_equal(net.set_dns_calls, 1, "resolvers are installed by this module, not the adapter");
	zassert_equal(net.dns_count, 1);
	zassert_ok(network_confirm(txn.id, &err));

	zassert_ok(network_get_status(&status));
	zassert_equal(status.dns_count, 1);
	zassert_equal(status.dns[0].bytes[3], 1);
}

/* --- scan --------------------------------------------------------------- */

ZTEST(network_manager, test_scan_runs_and_returns_results)
{
	struct api_error err;
	char job_id[JOB_ID_MAX_LEN + 1];
	struct network_scan_results results;

	net.scan_results.count = 2;
	net.scan_results.items[0].rssi = -40;
	net.scan_results.items[0].ssid_len = 5;
	memcpy(net.scan_results.items[0].ssid, "cedar", 5);
	net.scan_results.items[0].security = DEVICE_CONFIG_WIFI_WPA3_SAE;
	net.scan_results.items[0].connect_supported = true;
	net.scan_results.items[1].connect_supported = false;

	zassert_ok(network_scan_begin(NULL, 0, &err, job_id));
	zassert_equal(network_scan_results_get(job_id, &results), -EAGAIN);

	zassert_ok(network_scan_execute(job_id));
	zassert_ok(network_scan_results_get(job_id, &results));
	zassert_equal(results.count, 2);
	zassert_false(results.truncated);
	zassert_equal(results.items[0].rssi, -40);
	zassert_false(results.items[1].connect_supported,
		      "a network we cannot join stays visible but unselectable");
}

/*
 * An adapter is trusted to fill the array but not to count it: a count past
 * the end would be read as valid entries by everything downstream.
 */
ZTEST(network_manager, test_scan_results_are_capped_and_the_client_told)
{
	struct api_error err;
	char job_id[JOB_ID_MAX_LEN + 1];
	struct network_scan_results results;

	net.scan_results.count = 255;

	zassert_ok(network_scan_begin(NULL, 0, &err, job_id));
	zassert_ok(network_scan_execute(job_id));
	zassert_ok(network_scan_results_get(job_id, &results));
	zassert_equal(results.count, CONFIG_NETWORK_MANAGER_SCAN_MAX_RESULTS);
	zassert_true(results.truncated, "a partial list must not look complete");
}

ZTEST(network_manager, test_scan_is_refused_during_an_apply)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	char job_id[JOB_ID_MAX_LEN + 1];

	/* A staged candidate has changed nothing, so scanning is still fine. */
	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_scan_begin(NULL, 0, &err, job_id));

	zassert_ok(network_apply(txn.id, 0, &err));
	zassert_equal(network_scan_begin(NULL, 0, &err, job_id), -EBUSY,
		      "a scan takes the radio off the channel it is associated on");
	zassert_equal(err.code, API_ERR_BUSY);

	zassert_ok(network_apply_execute(txn.id));
	zassert_equal(network_scan_begin(NULL, 0, &err, job_id), -EBUSY);

	zassert_ok(network_confirm(txn.id, &err));
	zassert_ok(network_scan_begin(NULL, 0, &err, job_id));
}

ZTEST(network_manager, test_repeating_a_scan_request_returns_the_same_job)
{
	struct api_error err;
	char first[JOB_ID_MAX_LEN + 1];
	char second[JOB_ID_MAX_LEN + 1];

	zassert_ok(network_scan_begin("idem-key-0123456789", 7, &err, first));
	zassert_ok(network_scan_begin("idem-key-0123456789", 7, &err, second));
	zassert_equal(strcmp(first, second), 0, "a retry must not start a second scan");

	zassert_equal(network_scan_begin("idem-key-0123456789", 99, &err, second), -EINVAL);
	zassert_equal(err.code, API_ERR_IDEMPOTENCY_CONFLICT);
}

ZTEST(network_manager, test_scan_without_a_radio_is_unavailable)
{
	struct api_error err;
	char job_id[JOB_ID_MAX_LEN + 1];

	net.no_radio = true;
	bring_up();

	zassert_equal(network_scan_begin(NULL, 0, &err, job_id), -ENOTSUP);
	zassert_equal(err.code, API_ERR_CAPABILITY_UNAVAILABLE);
}

/* --- wire names --------------------------------------------------------- */

/* These strings are the HTTP contract; a typo here is a broken API. */
ZTEST(network_manager, test_state_names_match_the_contract)
{
	zassert_equal(strcmp(network_transaction_state_str(NETWORK_TXN_STAGED), "staged"), 0);
	zassert_equal(strcmp(network_transaction_state_str(NETWORK_TXN_APPLYING), "applying"), 0);
	zassert_equal(strcmp(network_transaction_state_str(NETWORK_TXN_AWAITING_CONFIRMATION),
			     "awaiting_confirmation"),
		      0);
	zassert_equal(strcmp(network_transaction_state_str(NETWORK_TXN_COMMITTED), "committed"),
		      0);
	zassert_equal(strcmp(network_transaction_state_str(NETWORK_TXN_ROLLING_BACK),
			     "rolling_back"),
		      0);
	zassert_equal(strcmp(network_transaction_state_str(NETWORK_TXN_ROLLED_BACK),
			     "rolled_back"),
		      0);
	zassert_equal(strcmp(network_transaction_state_str(NETWORK_TXN_FAILED), "failed"), 0);
	zassert_equal(strcmp(network_transaction_state_str(NETWORK_TXN_EXPIRED), "expired"), 0);

	for (int s = 0; s < NETWORK_TXN_STATE_COUNT; s++) {
		zassert_not_null(network_transaction_state_str(s), "state %d has no wire name", s);
	}
	zassert_is_null(network_transaction_state_str(NETWORK_TXN_STATE_COUNT));

	zassert_true(network_transaction_state_is_terminal(NETWORK_TXN_COMMITTED));
	zassert_true(network_transaction_state_is_terminal(NETWORK_TXN_ROLLED_BACK));
	zassert_true(network_transaction_state_is_terminal(NETWORK_TXN_FAILED));
	zassert_true(network_transaction_state_is_terminal(NETWORK_TXN_EXPIRED));
	zassert_false(network_transaction_state_is_terminal(NETWORK_TXN_STAGED));
	zassert_false(network_transaction_state_is_terminal(NETWORK_TXN_AWAITING_CONFIRMATION));
}

ZTEST(network_manager, test_init_rejects_an_incomplete_adapter)
{
	struct network_iface_ops broken;

	fake_net_bind(&net, &broken);
	broken.configure = NULL;
	zassert_equal(network_manager_init(&broken), -EINVAL);
	zassert_equal(network_manager_init(NULL), -EINVAL);
}
