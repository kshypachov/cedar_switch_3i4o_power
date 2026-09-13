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
 * interfaces. They are the first four groups below. P4 added the worker's half
 * (what network_manager_process() does once a request has been accepted, and
 * what happens when a request arrives while it works), the status the web
 * layer serialises, the default-route and resolver policies, the boot path and
 * the factory restore.
 *
 * Field paths and codes are asserted exactly as the mock produces them
 * (tools/api-contract/tests/test_mock_network.py), because the frontend reads
 * them from the mock first.
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
static char job[JOB_ID_MAX_LEN + 1];

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

static void to_dhcp(struct device_config_ipv4 *ipv4)
{
	memset(ipv4, 0, sizeof(*ipv4));
	ipv4->mode = DEVICE_CONFIG_IPV4_DHCP;
}

static int stage(const struct network_config_input *in, struct api_error *err,
		 struct network_transaction *txn)
{
	return network_stage_config(in, device_config_revision(), err, txn);
}

static int apply(const char *id, uint16_t timeout, struct api_error *err)
{
	return network_apply(id, timeout, NULL, 0, err, job);
}

static int confirm(const char *id, struct api_error *err)
{
	return network_confirm(id, err, job);
}

static int rollback(const char *id, struct api_error *err)
{
	return network_rollback(id, NULL, 0, err, job);
}

static enum network_transaction_state state_of(const char *id)
{
	struct network_transaction txn;

	zassert_ok(network_transaction_get(id, &txn));
	return txn.state;
}

static enum job_state job_state_of(const char *id)
{
	struct job_snapshot snapshot;

	zassert_ok(job_get(id, &snapshot));
	return snapshot.state;
}

/* Stage, apply and run the worker, leaving the transaction awaiting confirmation. */
static void apply_to_awaiting(const struct network_config_input *in,
			      struct network_transaction *txn)
{
	struct api_error err;

	zassert_ok(stage(in, &err, txn));
	zassert_ok(apply(txn->id, 0, &err));
	zassert_true(network_manager_process() > 0);
	zassert_ok(network_transaction_get(txn->id, txn));
	zassert_equal(txn->state, NETWORK_TXN_AWAITING_CONFIRMATION);
}

/* Confirm and let the worker write the commit. */
static void confirm_and_commit(struct network_transaction *txn)
{
	struct api_error err;

	zassert_ok(confirm(txn->id, &err));
	zassert_true(network_manager_process() > 0);
	zassert_ok(network_transaction_get(txn->id, txn));
	zassert_equal(txn->state, NETWORK_TXN_COMMITTED);
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
	zassert_ok(rollback(txn.id, &err));
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

/* The error names exactly this one field. */
static bool only_field(const struct api_error *err, const char *path, enum api_field_code code)
{
	return err->field_count == 1 && has_field(err, path, code);
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
	zassert_equal(txn.remaining_seconds, CONFIG_NETWORK_MANAGER_CANDIDATE_TTL_SECONDS);
	zassert_equal(network_manager_process(), 0, "nothing was asked of the worker");
	zassert_equal(net.eth.configure_calls, 0, "staging must not touch the hardware");
}

ZTEST(network_manager, test_static_without_an_address_names_both_missing_fields)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	memset(in.config.ethernet.ipv4.address, 0, 4);
	in.config.ethernet.ipv4.prefix_length = 0;
	in.config.ethernet.ipv4.has_gateway = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_equal(err.code, API_ERR_VALIDATION_FAILED);
	zassert_equal(err.field_count, 2);
	zassert_str_equal(err.fields[0].path, "/config/interfaces/ethernet/ipv4/address");
	zassert_equal(err.fields[0].code, API_FIELD_REQUIRED);
	zassert_str_equal(err.fields[1].path, "/config/interfaces/ethernet/ipv4/prefix_length");
	zassert_equal(err.fields[1].code, API_FIELD_REQUIRED);
}

ZTEST(network_manager, test_an_address_given_as_zero_is_an_address)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	/* The request said 0.0.0.0, not null: it is judged, not missing. */
	memset(in.config.ethernet.ipv4.address, 0, 4);
	in.config.ethernet.ipv4.prefix_length = 8;
	in.config.ethernet.ipv4.has_gateway = 0;
	in.ethernet_address_given = true;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/address",
				API_FIELD_OUT_OF_RANGE));
}

ZTEST(network_manager, test_prefix_bounds)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.ethernet.ipv4.prefix_length = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/prefix_length",
				API_FIELD_REQUIRED));

	in.config.ethernet.ipv4.prefix_length = 31;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/prefix_length",
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
		uint8_t prefix;
		const char *why;
	} bad[] = {
		{{192, 168, 88, 0}, 24, "the subnet address"},
		{{192, 168, 88, 255}, 24, "the broadcast address"},
		{{0, 1, 2, 3}, 8, "this network"},
		{{127, 0, 0, 5}, 8, "loopback"},
		{{169, 254, 3, 4}, 16, "link-local, which a failed DHCP assigns itself"},
		{{224, 0, 0, 5}, 24, "multicast"},
		{{240, 1, 2, 3}, 8, "class E"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		struct network_config_input in = valid_input();
		struct api_error err;

		memcpy(in.config.ethernet.ipv4.address, bad[i].addr, 4);
		in.config.ethernet.ipv4.prefix_length = bad[i].prefix;
		in.config.ethernet.ipv4.has_gateway = 0;
		zassert_equal(stage(&in, &err, NULL), -EINVAL, "accepted %s", bad[i].why);
		zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/address",
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
	zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/gateway",
				API_FIELD_OUT_OF_RANGE));

	/* The interface's own address is not a gateway. */
	in = valid_input();
	memcpy(in.config.ethernet.ipv4.gateway, in.config.ethernet.ipv4.address, 4);
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/gateway",
				API_FIELD_CONFLICTING));

	/* The subnet's own address is no better as a gateway. */
	in = valid_input();
	in.config.ethernet.ipv4.gateway[3] = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/gateway",
				API_FIELD_OUT_OF_RANGE));
}

/*
 * Wi-Fi's IPv4 block is reported under Wi-Fi. The path is what a form uses to
 * mark the field, and an error named under Ethernet would mark the wrong one.
 * Found by mutation: every IPv4 case so far was on Ethernet.
 */
ZTEST(network_manager, test_wifi_ipv4_errors_name_the_wifi_block)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	in.config.wifi.ipv4.mode = DEVICE_CONFIG_IPV4_STATIC;
	in.config.wifi.ipv4.prefix_length = 24;
	memcpy(in.config.wifi.ipv4.address, (uint8_t[]){192, 168, 89, 20}, 4);
	in.config.wifi.ipv4.has_gateway = 1;
	memcpy(in.config.wifi.ipv4.gateway, (uint8_t[]){10, 0, 0, 1}, 4);
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/wifi/ipv4/gateway",
				API_FIELD_OUT_OF_RANGE));
}

ZTEST(network_manager, test_a_gateway_is_not_judged_against_a_wrong_address)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.ethernet.ipv4.address[3] = 255;
	in.config.ethernet.ipv4.gateway[0] = 10;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/address",
				API_FIELD_OUT_OF_RANGE),
		     "the second message would be noise");
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
	zassert_equal(err.field_count, 3);
	zassert_str_equal(err.fields[0].path, "/config/interfaces/ethernet/ipv4/address");
	zassert_str_equal(err.fields[1].path, "/config/interfaces/ethernet/ipv4/prefix_length");
	zassert_str_equal(err.fields[2].path, "/config/interfaces/ethernet/ipv4/gateway");
	for (uint8_t i = 0; i < 3; i++) {
		zassert_equal(err.fields[i].code, API_FIELD_NOT_ALLOWED);
	}

	/* An address given as 0.0.0.0 is still given. */
	struct network_config_input zero = valid_input();

	to_dhcp(&zero.config.ethernet.ipv4);
	zero.ethernet_address_given = true;
	zassert_equal(stage(&zero, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/address",
				API_FIELD_NOT_ALLOWED));
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
	zassert_true(only_field(&err, "/config/dns/servers", API_FIELD_NOT_ALLOWED));
}

ZTEST(network_manager, test_dns_manual_needs_at_least_one_server)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL,
		      "manual with no resolver is not the same as automatic");
	zassert_true(only_field(&err, "/config/dns/servers", API_FIELD_REQUIRED));
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

	/* Three is past the limit the plan sets for v1, as the schema says. */
	in.config.dns.server_count = 3;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/dns/servers", API_FIELD_OUT_OF_RANGE));
}

ZTEST(network_manager, test_dns_rejects_the_unspecified_address_by_index)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 2;
	in.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET;
	in.config.dns.servers[1].family = DEVICE_CONFIG_AF_INET6;
	zassert_equal(stage(&in, &err, NULL), -EINVAL, "0.0.0.0 and :: are not resolvers");
	zassert_equal(err.field_count, 2);
	zassert_str_equal(err.fields[0].path, "/config/dns/servers/0");
	zassert_equal(err.fields[0].code, API_FIELD_INVALID_FORMAT);
	zassert_str_equal(err.fields[1].path, "/config/dns/servers/1");
	zassert_equal(err.fields[1].code, API_FIELD_INVALID_FORMAT);
}

/*
 * Only the all-zero address is the unspecified one, and an IPv6 address is
 * sixteen bytes: an IPv4-mapped resolver starts with four zero bytes and is a
 * perfectly good server. Found by mutation: looking at four bytes broke
 * nothing, because the only IPv6 resolver tested began 2001.
 */
ZTEST(network_manager, test_an_ipv6_resolver_starting_with_zeros_is_accepted)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 1;
	in.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET6;
	/* ::ffff:192.168.88.1 */
	in.config.dns.servers[0].bytes[10] = 0xff;
	in.config.dns.servers[0].bytes[11] = 0xff;
	in.config.dns.servers[0].bytes[12] = 192;
	in.config.dns.servers[0].bytes[13] = 168;
	in.config.dns.servers[0].bytes[14] = 88;
	in.config.dns.servers[0].bytes[15] = 1;
	zassert_ok(stage(&in, &err, NULL), "::ffff:192.168.88.1 is not ::");
}

/*
 * A resolver of a family the store does not know is not a resolver, whatever
 * its bytes. The web layer only produces the two families, so this guards the
 * module's own API. Found by mutation: dropping the family check broke nothing.
 */
ZTEST(network_manager, test_a_resolver_of_an_unknown_family_is_refused)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 1;
	in.config.dns.servers[0].family = 99;
	memset(in.config.dns.servers[0].bytes, 1, sizeof(in.config.dns.servers[0].bytes));
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/dns/servers/0", API_FIELD_INVALID_FORMAT));
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
	zassert_true(has_field(&err, "/config/interfaces/ethernet/enabled",
			       API_FIELD_CONFLICTING));
	zassert_true(has_field(&err, "/config/interfaces/wifi/enabled", API_FIELD_CONFLICTING));
}

ZTEST(network_manager, test_preferred_must_be_enabled)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.preferred_interface = DEVICE_CONFIG_INTERFACE_WIFI;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/preferred_interface", API_FIELD_CONFLICTING));
}

ZTEST(network_manager, test_wifi_cannot_be_enabled_without_the_radio)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	/*
	 * Confirm waits for every enabled interface, so a Wi-Fi that cannot come
	 * up would make this change impossible to confirm.
	 */
	net.wifi.present = false;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/wifi/enabled", API_FIELD_NOT_ALLOWED));
}

/*
 * The mock's order, which decides the six fields a client is told about. This
 * is test_the_field_list_is_bounded_and_says_so of the mock's suite: seven
 * problems, the first six kept, the truncation marked.
 */
ZTEST(network_manager, test_fields_come_in_the_mocks_order_and_are_bounded)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	in.config.preferred_interface = DEVICE_CONFIG_INTERFACE_WIFI;
	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 0;
	in.config.ethernet.enabled = 0;
	in.config.ethernet.ipv4.mode = DEVICE_CONFIG_IPV4_DHCP;
	in.config.wifi.enabled = 0;

	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_equal(CONFIG_API_VALIDATION_MAX_FIELDS, 6, "the mock's MAX_FIELDS");
	zassert_equal(err.field_count, 6);
	zassert_true(err.fields_truncated, "the seventh, preferred_interface, was dropped");

	const char *const expected[] = {
		"/config/interfaces/ethernet/enabled",
		"/config/interfaces/wifi/enabled",
		"/config/interfaces/ethernet/ipv4/address",
		"/config/interfaces/ethernet/ipv4/prefix_length",
		"/config/interfaces/ethernet/ipv4/gateway",
		"/config/dns/servers",
	};

	for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
		zassert_str_equal(err.fields[i].path, expected[i], "field %zu", i);
	}
}

/*
 * preferred_interface is judged before the Wi-Fi rules, the order the mock adds
 * them in: with only six fields kept, the order decides which ones a client is
 * told about. Found by mutation: swapping the two broke nothing, because no case
 * produced both.
 */
ZTEST(network_manager, test_preferred_interface_comes_before_the_wifi_rules)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	in.config.ethernet.enabled = 0; /* the preferred interface, now off */
	in.config.wifi.ssid_len = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_equal(err.field_count, 2);
	zassert_str_equal(err.fields[0].path, "/config/preferred_interface");
	zassert_equal(err.fields[0].code, API_FIELD_CONFLICTING);
	zassert_str_equal(err.fields[1].path, "/config/interfaces/wifi/ssid_base64");
	zassert_equal(err.fields[1].code, API_FIELD_REQUIRED);
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
	zassert_true(has_field(&err, "/config/interfaces/ethernet/enabled",
			       API_FIELD_NOT_ALLOWED));
	zassert_true(has_field(&err, "/config/interfaces/wifi/enabled", API_FIELD_NOT_ALLOWED));

	/* An associated Wi-Fi is a perfectly good recovery path. */
	struct network_config_input wifi = wifi_input();

	net.wifi.link_up = true;
	zassert_ok(stage(&wifi, &err, NULL));
}

/*
 * An enabled Wi-Fi is a recovery path only once it has a link: a radio that is
 * present and has not associated reaches nobody. Found by mutation: counting a
 * Wi-Fi without link broke nothing, because the Wi-Fi case always had one.
 */
ZTEST(network_manager, test_a_wifi_without_a_link_is_no_recovery_path)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	net.eth.link_up = false;
	zassert_false(net.wifi.link_up, "present, not associated");
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(has_field(&err, "/config/interfaces/ethernet/enabled",
			       API_FIELD_NOT_ALLOWED));
	zassert_true(has_field(&err, "/config/interfaces/wifi/enabled", API_FIELD_NOT_ALLOWED));
}

/*
 * The recovery path is judged only on a proposal the other rules accept, so a
 * rejection names what is wrong with the request — the mock's field list — and
 * not also a missing link the operator may be about to fix by exactly this
 * change. Found by mutation: judging it regardless broke nothing.
 */
ZTEST(network_manager, test_the_recovery_path_does_not_add_to_other_refusals)
{
	struct network_config_input in = valid_input();
	struct api_error err;

	net.eth.link_up = false;
	in.config.ethernet.ipv4.prefix_length = 31;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/ethernet/ipv4/prefix_length",
				API_FIELD_OUT_OF_RANGE));
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

ZTEST(network_manager, test_enabled_wifi_needs_a_valid_ssid)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	in.config.wifi.ssid_len = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/wifi/ssid_base64", API_FIELD_REQUIRED));

	in.config.wifi.ssid_len = DEVICE_CONFIG_SSID_MAX_LEN + 1;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/wifi/ssid_base64",
				API_FIELD_OUT_OF_RANGE),
		     "33 bytes pass the schema's 44 base64 characters, not the rule");

	in = wifi_input();
	in.ssid_invalid = true;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/wifi/ssid_base64",
				API_FIELD_INVALID_FORMAT));
}

ZTEST(network_manager, test_protected_network_needs_a_password)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	in.wifi_password.action = DEVICE_CONFIG_SECRET_KEEP;
	in.wifi_password.value = NULL;
	in.wifi_password.len = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL, "nothing is stored to keep");
	zassert_true(only_field(&err, "/config/interfaces/wifi/credential/action",
				API_FIELD_CONFLICTING));
}

/* A replace without a value is the schema's oneOf failing: the whole object. */
ZTEST(network_manager, test_a_replace_without_a_value_is_conflicting)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	in.wifi_password.value = NULL;
	in.wifi_password.len = 0;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/wifi/credential", API_FIELD_CONFLICTING));
}

/*
 * A password longer than the store holds is refused, not cut to fit: a
 * truncated password is a different password, and the profile would silently
 * fail to associate. The web schema stops earlier, so this guards the module's
 * own API. Found by mutation: dropping the length bound broke nothing.
 */
ZTEST(network_manager, test_a_replace_longer_than_the_store_holds_is_conflicting)
{
	static const uint8_t too_long[DEVICE_CONFIG_SECRET_MAX_LEN + 1] = {[0 ... DEVICE_CONFIG_SECRET_MAX_LEN] = 'p'};
	struct network_config_input in = wifi_input();
	struct api_error err;

	in.wifi_password.value = too_long;
	in.wifi_password.len = sizeof(too_long);
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/wifi/credential", API_FIELD_CONFLICTING));

	in.wifi_password.len = DEVICE_CONFIG_SECRET_MAX_LEN;
	zassert_ok(stage(&in, &err, NULL), "exactly what the store holds is fine");
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
	confirm_and_commit(&txn);

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
	zassert_true(only_field(&err, "/config/interfaces/wifi/credential/action",
				API_FIELD_NOT_ALLOWED));

	/* So is a different security mode. */
	struct network_config_input resecured = again;

	resecured.config.wifi.security = DEVICE_CONFIG_WIFI_WPA2_PSK;
	zassert_equal(stage(&resecured, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/wifi/credential/action",
				API_FIELD_NOT_ALLOWED));
}

/*
 * A kept password is still a set password. The candidate a client reads back
 * has to say so, or the form asks again for a password that is stored. Found by
 * mutation: password_set of a KEEP candidate was never read back.
 */
ZTEST(network_manager, test_a_kept_password_is_reported_as_set)
{
	struct network_config_input in = wifi_input();
	struct network_transaction txn;
	struct api_error err;

	net.wifi.link_up = true;
	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);

	in.wifi_password.action = DEVICE_CONFIG_SECRET_KEEP;
	in.wifi_password.value = NULL;
	in.wifi_password.len = 0;
	zassert_ok(stage(&in, &err, &txn));
	zassert_equal(txn.candidate.wifi.password_set, 1, "the committed password is kept");
}

ZTEST(network_manager, test_open_network_credential_rules)
{
	struct network_config_input in = wifi_input();
	struct api_error err;

	/* A password offered for an open network is a contradiction. */
	in.config.wifi.security = DEVICE_CONFIG_WIFI_OPEN;
	zassert_equal(stage(&in, &err, NULL), -EINVAL);
	zassert_true(only_field(&err, "/config/interfaces/wifi/credential/action",
				API_FIELD_NOT_ALLOWED));

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
	zassert_true(only_field(&err, "/config/interfaces/wifi/credential/action",
				API_FIELD_CONFLICTING));
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
	confirm_and_commit(&txn);
	zassert_equal(device_config_revision(), 1);

	zassert_equal(network_stage_config(&in, 0, &err, NULL), -EINVAL);
	zassert_equal(err.code, API_ERR_STALE_REVISION,
		      "a candidate built on revision 0 must not silently undo revision 1");
	zassert_not_null(strstr(err.message, "revision 1"), "it says what to re-read");
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
	zassert_ok(rollback(txn.id, &err));
	zassert_ok(stage(&in, &err, NULL));
}

/* --- the transaction ---------------------------------------------------- */

ZTEST(network_manager, test_full_transaction_reaches_committed)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config cfg;
	struct device_config_pending_info pending;

	zassert_ok(stage(&in, &err, &txn));
	zassert_equal(device_config_revision(), 0);

	zassert_ok(apply(txn.id, 0, &err));
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_APPLYING);
	zassert_true(txn.has_job, "an accepted change is tracked by a job");
	zassert_str_equal(txn.job_id, job);
	zassert_equal(job_state_of(job), JOB_STATE_RUNNING);
	zassert_equal(device_config_revision(), 0, "apply must not commit");
	zassert_equal(net.eth.configure_calls, 0,
		      "the network must not change before the response is sent");

	/* The journal is durable before anything is touched. */
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_true(pending.present);
	zassert_str_equal(pending.transaction_id, txn.id);

	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_AWAITING_CONFIRMATION);
	zassert_equal(job_state_of(job), JOB_STATE_WAITING_CONFIRMATION);
	zassert_equal(net.eth.configure_calls, 1);
	zassert_equal(net.eth.applied.address[3], 14);
	zassert_equal(device_config_revision(), 0, "still not committed");

	zassert_ok(confirm(txn.id, &err));
	zassert_str_equal(job, txn.job_id, "confirm answers with the apply job");
	zassert_equal(state_of(txn.id), NETWORK_TXN_AWAITING_CONFIRMATION,
		      "the commit is written by the worker, after the 202");
	zassert_equal(job_state_of(job), JOB_STATE_RUNNING);
	zassert_equal(device_config_revision(), 0);

	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_COMMITTED);
	zassert_equal(device_config_revision(), 1);

	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 14);

	/* The journal is gone, and the job succeeded. */
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);
	zassert_equal(job_state_of(txn.job_id), JOB_STATE_SUCCEEDED);
}

ZTEST(network_manager, test_confirming_twice_before_the_commit_is_the_same_job)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	char first[JOB_ID_MAX_LEN + 1];

	apply_to_awaiting(&in, &txn);
	zassert_ok(confirm(txn.id, &err));
	memcpy(first, job, sizeof(first));
	zassert_ok(confirm(txn.id, &err), "a retry that lost its response");
	zassert_str_equal(job, first);
	zassert_true(network_manager_process() > 0);
	zassert_equal(device_config_revision(), 1, "committed once");
}

/*
 * A confirm that was accepted is not judged again. The commit is only waiting
 * for the worker, so a retry that lost its response gets the same job even if
 * an interface stopped looking healthy in between — answering it "not every
 * interface is working yet" would tell the client its accepted confirm failed.
 * Found by mutation: removing the replay branch broke nothing, because every
 * retry so far happened while the interfaces were still healthy.
 */
ZTEST(network_manager, test_a_confirm_retry_before_the_commit_is_not_judged_again)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	char first[JOB_ID_MAX_LEN + 1];

	apply_to_awaiting(&in, &txn);
	zassert_ok(confirm(txn.id, &err));
	memcpy(first, job, sizeof(first));

	net.eth.suppress_route = true;
	zassert_ok(confirm(txn.id, &err), "the retry of an accepted confirm is not refused");
	zassert_str_equal(job, first, "and it is answered with the same job");

	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_COMMITTED);
}

ZTEST(network_manager, test_the_committed_configuration_survives_a_reboot)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;
	struct device_config_recovery_report report;
	struct device_config cfg;

	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);

	zassert_ok(device_config_init(&store_backend, &report));
	zassert_equal(report.result, DEVICE_CONFIG_RECOVERY_CLEAN);
	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_equal(cfg.ethernet.ipv4.address[3], 14);
}

ZTEST(network_manager, test_discarding_a_staged_candidate_has_its_own_job)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config_pending_info pending;
	struct job_snapshot snapshot;

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(rollback(txn.id, &err));

	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_ROLLED_BACK);
	zassert_false(txn.has_error);
	zassert_str_equal(txn.job_id, job);
	zassert_ok(job_get(job, &snapshot));
	zassert_equal(snapshot.kind, JOB_KIND_NETWORK_DISCARD);
	zassert_equal(snapshot.state, JOB_STATE_SUCCEEDED);

	zassert_equal(net.eth.configure_calls, 0);
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);
	zassert_equal(device_config_revision(), 0);
}

ZTEST(network_manager, test_a_discard_is_replayed_by_its_key)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	char first[JOB_ID_MAX_LEN + 1];

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_rollback(txn.id, "discard-key-0123456789", 5, &err, job));
	memcpy(first, job, sizeof(first));

	zassert_ok(network_rollback(txn.id, "discard-key-0123456789", 5, &err, job),
		   "the retry of an accepted discard is not refused as already finished");
	zassert_str_equal(job, first);

	zassert_equal(network_rollback(txn.id, "discard-key-0123456789", 6, &err, job), -EINVAL);
	zassert_equal(err.code, API_ERR_IDEMPOTENCY_CONFLICT);
}

ZTEST(network_manager, test_rollback_after_apply_restores_the_interfaces)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config_pending_info pending;

	apply_to_awaiting(&in, &txn);
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_STATIC);

	zassert_ok(rollback(txn.id, &err));
	zassert_str_equal(job, txn.job_id, "an applied change rolls back on its apply job");
	zassert_equal(state_of(txn.id), NETWORK_TXN_ROLLING_BACK);
	zassert_equal(job_state_of(job), JOB_STATE_RUNNING);
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_STATIC,
		      "the handler only records it; the worker restores");

	zassert_true(network_manager_process() > 0);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_ROLLED_BACK);
	zassert_false(txn.has_error);

	/* Back to the committed generation, which is still the DHCP default. */
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP);
	zassert_equal(device_config_revision(), 0);
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);
	zassert_equal(job_state_of(txn.job_id), JOB_STATE_SUCCEEDED,
		      "a rollback the administrator asked for is a success");
}

ZTEST(network_manager, test_a_rollback_before_the_push_touches_nothing)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config_pending_info pending;

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(apply(txn.id, 0, &err));
	zassert_ok(rollback(txn.id, &err));

	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_ROLLED_BACK);
	zassert_equal(net.eth.configure_calls, 0,
		      "a candidate that never reached the interfaces has nothing to undo");
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);
}

static const char *during_push_id;

static void rollback_during_push(void *arg)
{
	struct api_error err;
	char id[JOB_ID_MAX_LEN + 1];

	ARG_UNUSED(arg);
	zassert_ok(network_rollback(during_push_id, NULL, 0, &err, id),
		   "a request is answered while the worker pushes");
}

ZTEST(network_manager, test_a_rollback_asked_for_during_the_push_undoes_it)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(apply(txn.id, 0, &err));
	during_push_id = txn.id;
	net.during_io = rollback_during_push;

	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_ROLLED_BACK,
		      "the push finished, then the same pass undid it");
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP);
	zassert_equal(job_state_of(job), JOB_STATE_SUCCEEDED, "on the apply job");
}

ZTEST(network_manager, test_a_confirmed_change_can_no_longer_be_rolled_back)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	apply_to_awaiting(&in, &txn);
	zassert_ok(confirm(txn.id, &err));
	zassert_equal(rollback(txn.id, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INVALID_STATE);
	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_COMMITTED);
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

	zassert_equal(apply(txn.id, 0, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INTERNAL_ERROR);

	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_FAILED);
	zassert_equal(job_state_of(txn.job_id), JOB_STATE_FAILED);
	(void)network_manager_process();
	zassert_equal(net.eth.configure_calls, 0);
	zassert_equal(device_config_revision(), 0);
}

ZTEST(network_manager, test_an_adapter_failure_restores_the_committed_config)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	struct device_config_pending_info pending;
	struct job_snapshot snapshot;

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(apply(txn.id, 0, &err));

	net.fail_configure = -EIO;
	zassert_true(network_manager_process() > 0);

	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_FAILED);
	zassert_true(txn.has_error);
	zassert_equal(txn.error_code, API_ERR_INTERNAL_ERROR);
	zassert_ok(job_get(txn.job_id, &snapshot));
	zassert_equal(snapshot.state, JOB_STATE_FAILED);
	zassert_str_equal(snapshot.error.code, "internal_error");

	/* Nothing is left behind: not on the hardware, not in the journal. */
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP);
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);
	zassert_equal(device_config_revision(), 0);
}

ZTEST(network_manager, test_a_commit_that_cannot_be_written_is_rolled_back)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	apply_to_awaiting(&in, &txn);
	zassert_ok(confirm(txn.id, &err));
	/* The first real commit lands in slot A (device_config_store.c). */
	storage.fail_write_slot = DEVICE_CONFIG_SLOT_COMMITTED_A;
	storage.fail_write_errno = -EIO;

	zassert_true(network_manager_process() > 0);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_FAILED,
		      "a change whose next reboot would return to something else is not kept");
	zassert_equal(txn.error_code, API_ERR_INTERNAL_ERROR);
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP);
	zassert_equal(device_config_revision(), 0);
	zassert_equal(job_state_of(txn.job_id), JOB_STATE_FAILED);
}

ZTEST(network_manager, test_state_transitions_are_guarded)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));

	/* Confirming something that was never applied. */
	zassert_equal(confirm(txn.id, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INVALID_STATE);
	zassert_not_null(strstr(err.message, "staged"), "the message names the state");

	zassert_ok(apply(txn.id, 0, &err));

	/* Applying twice. */
	zassert_equal(apply(txn.id, 0, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INVALID_STATE);

	zassert_true(network_manager_process() > 0);
	confirm_and_commit(&txn);

	/* And nothing works once it is finished. */
	zassert_equal(confirm(txn.id, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INVALID_STATE);
	zassert_equal(rollback(txn.id, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INVALID_STATE);
}

ZTEST(network_manager, test_unknown_transaction_ids)
{
	struct api_error err;
	struct network_transaction txn;

	zassert_equal(network_transaction_get("txn_nope", &txn), -ENOENT);
	zassert_equal(network_transaction_current(&txn), -ENOENT);

	zassert_equal(apply("txn_nope", 0, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_NOT_FOUND);
	zassert_equal(confirm("txn_nope", &err), -EINVAL);
	zassert_equal(err.code, API_ERR_NOT_FOUND);
	zassert_equal(rollback("txn_nope", &err), -EINVAL);
	zassert_equal(err.code, API_ERR_NOT_FOUND);
}

ZTEST(network_manager, test_an_apply_is_replayed_by_its_key_whatever_happened_since)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	char first[JOB_ID_MAX_LEN + 1];

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_apply(txn.id, 120, "apply-key-0123456789", 9, &err, job));
	memcpy(first, job, sizeof(first));
	zassert_true(network_manager_process() > 0);

	zassert_ok(network_apply(txn.id, 120, "apply-key-0123456789", 9, &err, job),
		   "the transaction is no longer staged, and the retry is still answered");
	zassert_str_equal(job, first);

	zassert_equal(network_apply(txn.id, 90, "apply-key-0123456789", 10, &err, job), -EINVAL);
	zassert_equal(err.code, API_ERR_IDEMPOTENCY_CONFLICT);
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

	zassert_equal(confirm(txn.id, &err), -EAGAIN,
		      "Ethernet working says nothing about whether Wi-Fi joined");
	zassert_equal(err.code, API_ERR_INVALID_STATE);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_AWAITING_CONFIRMATION,
		      "not healthy yet is not the same as failed");
	zassert_false(txn.has_error);
	zassert_equal(network_manager_process(), 0, "nothing was asked of the worker");
	zassert_equal(device_config_revision(), 0);

	/* Once the radio does associate, the same call succeeds. */
	net.associated = true;
	net.wifi.link_up = true;
	confirm_and_commit(&txn);
	zassert_equal(device_config_revision(), 1);
}

ZTEST(network_manager, test_confirm_waits_for_dhcp)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	to_dhcp(&in.config.ethernet.ipv4);
	net.eth.dhcp_answers = false;
	apply_to_awaiting(&in, &txn);
	zassert_equal(confirm(txn.id, &err), -EAGAIN, "no lease, no address");

	net.eth.dhcp_answers = true;
	confirm_and_commit(&txn);
}

/*
 * Link and address alone would let a device with no way off its own subnet
 * look healthy, so a configuration that asked for a gateway needs the route.
 */
ZTEST(network_manager, test_a_requested_gateway_must_produce_a_route)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;

	in.config.ethernet.ipv4.has_gateway = 0;
	memset(in.config.ethernet.ipv4.gateway, 0, 4);
	apply_to_awaiting(&in, &txn);
	zassert_false(net.eth.has_route, "no gateway was configured");
	confirm_and_commit(&txn);
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

	zassert_equal(confirm(txn.id, &err), -EAGAIN);
	zassert_equal(device_config_revision(), 0);

	net.eth.suppress_route = false;
	confirm_and_commit(&txn);
	zassert_equal(device_config_revision(), 1);
}

ZTEST(network_manager, test_a_disabled_interface_is_not_health_checked)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;

	/* Wi-Fi is off and cannot associate; that must not block the commit. */
	net.wifi.wifi_associates = false;
	net.wifi.present = false;
	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);
	zassert_equal(net.disconnect_calls, 1, "a disabled radio is taken down");
	zassert_false(net.wifi.enabled);
}

/* --- the confirmation timer --------------------------------------------- */

ZTEST(network_manager, test_a_candidate_nobody_applies_expires)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));

	advance_seconds(CONFIG_NETWORK_MANAGER_CANDIDATE_TTL_SECONDS - 1);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_STAGED);
	zassert_equal(txn.remaining_seconds, 1);

	/* Reading it is enough: no worker has to run for the client to see it. */
	advance_seconds(2);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_EXPIRED);
	zassert_true(txn.has_error);
	zassert_equal(txn.error_code, API_ERR_RESOURCE_EXPIRED);
	zassert_equal(txn.remaining_seconds, -1);

	/* Applying it is refused as the state it is in, and the slot is free. */
	zassert_equal(apply(txn.id, 0, &err), -EINVAL);
	zassert_equal(err.code, API_ERR_INVALID_STATE);
	zassert_ok(stage(&in, &err, NULL));
}

/*
 * A deadline is an instant, and at that instant it has passed: the candidate is
 * expired and an unconfirmed change is rolling back, as the mock counts
 * (now - created >= TTL, now >= deadline). Found by mutation: moving the
 * boundary broke nothing, because every test stepped a second past it.
 */
ZTEST(network_manager, test_a_deadline_has_passed_at_its_instant)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));
	advance_seconds(CONFIG_NETWORK_MANAGER_CANDIDATE_TTL_SECONDS);
	zassert_equal(state_of(txn.id), NETWORK_TXN_EXPIRED, "expired at exactly its TTL");

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(apply(txn.id, CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MIN_SECONDS, &err));
	zassert_true(network_manager_process() > 0);
	advance_seconds(CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MIN_SECONDS);
	zassert_equal(state_of(txn.id), NETWORK_TXN_ROLLING_BACK,
		      "not confirmed by the deadline is not confirmed");
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
	struct job_snapshot snapshot;

	apply_to_awaiting(&in, &txn);
	zassert_equal(net.eth.applied.address[3], 14);

	advance_seconds(CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_SECONDS - 1);
	(void)network_manager_process();
	zassert_equal(state_of(txn.id), NETWORK_TXN_AWAITING_CONFIRMATION,
		      "the deadline has not passed yet");

	advance_seconds(2);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_ROLLING_BACK,
		      "a poll sees the rollback begin before the worker has run");
	zassert_equal(txn.error_code, API_ERR_RESOURCE_EXPIRED);
	zassert_equal(job_state_of(txn.job_id), JOB_STATE_RUNNING);

	zassert_true(network_manager_process() > 0);
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.state, NETWORK_TXN_ROLLED_BACK);
	zassert_true(txn.has_error);
	zassert_equal(txn.error_code, API_ERR_RESOURCE_EXPIRED);

	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP,
		      "the interfaces must be back on the committed configuration");
	zassert_equal(device_config_revision(), 0);
	zassert_ok(device_config_pending_info_get(&pending));
	zassert_false(pending.present);

	zassert_ok(job_get(txn.job_id, &snapshot));
	zassert_equal(snapshot.state, JOB_STATE_FAILED);
	zassert_str_equal(snapshot.error.code, "resource_expired");
}

/* A worker that never runs must not leave the change live forever either. */
ZTEST(network_manager, test_a_worker_that_never_runs_still_times_out)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(apply(txn.id, 0, &err));

	advance_seconds(CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_SECONDS + 1);
	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_ROLLED_BACK);
	zassert_equal(net.eth.configure_calls, 0, "the candidate was never pushed");
}

ZTEST(network_manager, test_a_confirmation_in_time_is_honoured_after_the_deadline)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	apply_to_awaiting(&in, &txn);
	zassert_ok(confirm(txn.id, &err));
	advance_seconds(CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_SECONDS + 5);

	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_COMMITTED,
		      "the worker was late, the client was not");
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

	zassert_ok(apply(txn.id, 90, &err));
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.remaining_seconds, 90,
		      "apply starts its own deadline, visible while applying");

	/* Rounded down, as the mock counts. */
	fake_now += 3400;
	zassert_ok(network_transaction_get(txn.id, &txn));
	zassert_equal(txn.remaining_seconds, 86);

	/* A finished transaction has no deadline to report. */
	zassert_true(network_manager_process() > 0);
	confirm_and_commit(&txn);
	zassert_equal(txn.remaining_seconds, -1);
}

ZTEST(network_manager, test_confirmation_timeout_is_bounded)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;

	zassert_ok(stage(&in, &err, &txn));

	zassert_equal(apply(txn.id, CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MIN_SECONDS - 1, &err),
		      -EINVAL, "a client that only has to reconnect would run out of time");
	zassert_true(only_field(&err, "/confirmation_timeout_seconds", API_FIELD_OUT_OF_RANGE));

	zassert_equal(apply(txn.id, CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MAX_SECONDS + 1, &err),
		      -EINVAL, "longer than anyone waits before power-cycling the device");

	zassert_ok(apply(txn.id, CONFIG_NETWORK_MANAGER_CONFIRM_TIMEOUT_MIN_SECONDS, &err));
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
	struct network_transaction txn;

	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);

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
	struct network_transaction txn;
	struct network_status status;

	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);

	zassert_ok(network_get_status(&status));
	zassert_true(status.ethernet.link_up);
	zassert_true(status.ethernet.has_ipv4);
	zassert_equal(status.ethernet.ipv4[3], 14);
	zassert_true(status.has_active);
	zassert_equal(status.active, DEVICE_CONFIG_INTERFACE_ETHERNET);
	zassert_mem_equal(status.ethernet.mac, net.eth.mac, 6);
	zassert_equal(status.ethernet.addr_count, 1);
	zassert_equal(status.ethernet.addrs[0].source, NETWORK_ADDR_STATIC);

	/* Pulling the cable is visible immediately: status is read through, not cached. */
	net.eth.link_up = false;
	zassert_ok(network_get_status(&status));
	zassert_false(status.ethernet.link_up);
	zassert_false(status.ethernet.has_ipv4);
	zassert_equal(status.ethernet.state, NETWORK_IFACE_DOWN);
	zassert_false(status.has_active, "nothing else can carry traffic");
}

ZTEST(network_manager, test_interface_state_follows_what_is_observed)
{
	struct network_status status;
	struct network_config_input in = wifi_input();
	struct network_transaction txn;
	struct api_error err;

	/* The committed default: Ethernet on DHCP, Wi-Fi disabled. */
	net.eth.configured = true;
	net.eth.enabled = true;
	zassert_ok(network_get_status(&status));
	zassert_true(status.ethernet.enabled);
	zassert_equal(status.ethernet.state, NETWORK_IFACE_READY);
	zassert_false(status.wifi.enabled);
	zassert_equal(status.wifi.state, NETWORK_IFACE_DISABLED);

	net.eth.dhcp_answers = false;
	zassert_ok(network_get_status(&status));
	zassert_equal(status.ethernet.state, NETWORK_IFACE_ADDRESSING, "link, no lease yet");

	/* Once a candidate is on the interfaces, its settings are what is in force. */
	net.eth.dhcp_answers = true;
	net.connect_pending = true;
	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(apply(txn.id, 0, &err));
	zassert_true(network_manager_process() > 0);
	zassert_ok(network_get_status(&status));
	zassert_true(status.wifi.enabled);
	zassert_equal(status.wifi.state, NETWORK_IFACE_CONNECTING);

	net.connecting = false;
	net.connect_failed = true;
	zassert_ok(network_get_status(&status));
	zassert_equal(status.wifi.state, NETWORK_IFACE_FAILED,
		      "a wrong password is failed, not merely down");

	net.connect_failed = false;
	zassert_ok(network_get_status(&status));
	zassert_equal(status.wifi.state, NETWORK_IFACE_DOWN);

	net.associated = true;
	net.wifi.link_up = true;
	net.wifi.dhcp_answers = false;
	zassert_ok(network_get_status(&status));
	zassert_equal(status.wifi.state, NETWORK_IFACE_ADDRESSING);

	net.wifi.present = false;
	zassert_ok(network_get_status(&status));
	zassert_equal(status.wifi.state, NETWORK_IFACE_FAILED, "an absent radio is failed");
}

ZTEST(network_manager, test_status_reports_manual_resolvers_without_get_dns)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;
	struct network_status status;

	net.no_get_dns = true;
	bring_up();

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
	confirm_and_commit(&txn);

	zassert_ok(network_get_status(&status));
	zassert_equal(status.dns_count, 1);
	zassert_equal(status.dns[0].bytes[3], 1);
}

ZTEST(network_manager, test_status_reports_the_resolvers_in_force)
{
	struct network_status status;

	/* Automatic DNS: whatever DHCP installed, which only the adapter knows. */
	net.dns_override = true;
	net.dns_seen_count = 2;
	net.dns_seen[0].family = DEVICE_CONFIG_AF_INET;
	net.dns_seen[0].bytes[0] = 9;
	net.dns_seen[1].family = DEVICE_CONFIG_AF_INET;
	net.dns_seen[1].bytes[0] = 8;

	zassert_ok(network_get_status(&status));
	zassert_equal(status.dns_count, 2);
	zassert_equal(status.dns[0].bytes[0], 9);
	zassert_equal(status.dns[1].bytes[0], 8);
}

/*
 * A staged candidate has changed nothing, so it is not what the status reports
 * and not what the policies keep in force. Found by mutation: treating it as in
 * force broke no test, because every status read came after the push.
 */
ZTEST(network_manager, test_a_staged_candidate_is_not_in_force)
{
	struct network_config_input in = wifi_input();
	struct network_transaction txn;
	struct network_status status;
	struct api_error err;

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_get_status(&status));
	zassert_false(status.wifi.enabled, "Wi-Fi is enabled only once the candidate is pushed");
	zassert_equal(status.wifi.state, NETWORK_IFACE_DISABLED);

	(void)network_manager_process();
	zassert_equal(net.connect_calls, 0, "nobody joins the network of a candidate not applied");
}

/* --- policies ----------------------------------------------------------- */

ZTEST(network_manager, test_the_default_route_prefers_and_falls_back)
{
	struct network_config_input in = wifi_input();
	struct network_transaction txn;

	/* Both interfaces working: the preferred one, set once. */
	net.wifi.link_up = true;
	apply_to_awaiting(&in, &txn);
	zassert_true(net.has_default);
	zassert_equal(net.default_iface, DEVICE_CONFIG_INTERFACE_ETHERNET);
	unsigned int calls = net.set_default_calls;

	(void)network_manager_process();
	zassert_equal(net.set_default_calls, calls, "only a change is applied");

	/* The cable comes out: traffic moves to Wi-Fi. */
	net.eth.link_up = false;
	zassert_true(network_manager_process() > 0);
	zassert_equal(net.default_iface, DEVICE_CONFIG_INTERFACE_WIFI);

	/* And back when it returns. */
	net.eth.link_up = true;
	zassert_true(network_manager_process() > 0);
	zassert_equal(net.default_iface, DEVICE_CONFIG_INTERFACE_ETHERNET);

	/* Neither working: nothing to set. */
	net.eth.link_up = false;
	net.associated = false;
	net.wifi.link_up = false;
	calls = net.set_default_calls;
	(void)network_manager_process();
	zassert_true(net.set_default_calls <= calls + 0U);
}

/*
 * Traffic never goes out of an interface the configuration disabled, even if
 * the adapter still reports it up — a radio that has not been taken down yet,
 * say. Found by mutation: ignoring "enabled" in the choice broke nothing,
 * because a disabled interface in the fake never had a link.
 */
ZTEST(network_manager, test_no_default_route_through_a_disabled_interface)
{
	struct network_status status;

	/* The committed default: Wi-Fi disabled. The radio still says it is up. */
	net.wifi.configured = true;
	net.wifi.enabled = true;
	net.wifi.link_up = true;
	net.associated = true;
	net.eth.link_up = false;

	zassert_ok(network_get_status(&status));
	zassert_true(status.wifi.has_ipv4, "the adapter reports an address");
	zassert_false(status.has_active, "and still nothing is chosen to carry traffic");

	(void)network_manager_process();
	zassert_false(net.has_default, "no default route is set through it");
}

ZTEST(network_manager, test_a_wifi_preference_is_honoured)
{
	struct network_config_input in = wifi_input();
	struct network_transaction txn;

	in.config.preferred_interface = DEVICE_CONFIG_INTERFACE_WIFI;
	net.wifi.link_up = true;
	apply_to_awaiting(&in, &txn);
	zassert_equal(net.default_iface, DEVICE_CONFIG_INTERFACE_WIFI);
}

ZTEST(network_manager, test_a_manual_resolver_list_is_kept_in_force)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 1;
	in.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET;
	in.config.dns.servers[0].bytes[0] = 192;
	in.config.dns.servers[0].bytes[1] = 168;
	in.config.dns.servers[0].bytes[2] = 88;
	in.config.dns.servers[0].bytes[3] = 1;
	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);
	unsigned int calls = net.set_dns_calls;

	/* A DHCP renewal replaced the list. */
	net.dns_override = true;
	net.dns_seen_count = 1;
	net.dns_seen[0].family = DEVICE_CONFIG_AF_INET;
	net.dns_seen[0].bytes[0] = 9;

	zassert_true(network_manager_process() > 0);
	zassert_equal(net.set_dns_calls, calls + 1U);
	zassert_equal(net.dns[0].bytes[3], 1, "the manual resolver is back");
	(void)network_manager_process();
	zassert_equal(net.set_dns_calls, calls + 1U, "and left alone once it is");
}

/*
 * An IPv6 resolver is compared on all sixteen bytes. Found by mutation:
 * comparing only four broke nothing, because the only list ever replaced was
 * IPv4 — and two IPv6 resolvers in one /32 differ only past the fourth byte.
 */
ZTEST(network_manager, test_a_manual_ipv6_resolver_is_kept_in_force)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;

	in.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	in.config.dns.server_count = 1;
	in.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET6;
	in.config.dns.servers[0].bytes[0] = 0x20;
	in.config.dns.servers[0].bytes[1] = 0x01;
	in.config.dns.servers[0].bytes[2] = 0x0d;
	in.config.dns.servers[0].bytes[3] = 0xb8;
	in.config.dns.servers[0].bytes[15] = 0x01;
	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);
	unsigned int calls = net.set_dns_calls;

	/* A router advertisement installed another server from the same /32. */
	net.dns_override = true;
	net.dns_seen_count = 1;
	net.dns_seen[0] = in.config.dns.servers[0];
	net.dns_seen[0].bytes[15] = 0x02;

	(void)network_manager_process();
	zassert_equal(net.set_dns_calls, calls + 1U, "2001:db8::2 is not the manual 2001:db8::1");
	zassert_equal(net.dns[0].bytes[15], 0x01, "the manual resolver is back");
}

/*
 * A Wi-Fi network that dropped is joined again, but not every second: five
 * seconds, then twice as late each time up to a minute, and from the first
 * step again once it has joined.
 */
ZTEST(network_manager, test_a_dropped_wifi_is_rejoined_with_backoff)
{
	struct network_config_input in = wifi_input();
	struct network_transaction txn;

	net.wifi.link_up = true;
	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);
	zassert_equal(net.connect_calls, 1);

	/* The AP goes away and refuses every attempt. */
	net.associated = false;
	net.wifi.link_up = false;
	net.wifi.wifi_associates = false;
	(void)network_manager_process();
	zassert_equal(net.connect_calls, 1, "the first retry waits five seconds");

	advance_seconds(6);
	zassert_true(network_manager_process() > 0);
	zassert_equal(net.connect_calls, 2);

	advance_seconds(9);
	(void)network_manager_process();
	zassert_equal(net.connect_calls, 2, "the next one waits ten");
	advance_seconds(2);
	(void)network_manager_process();
	zassert_equal(net.connect_calls, 3);

	for (int i = 0; i < 6; i++) {
		advance_seconds(61);
		(void)network_manager_process();
	}
	zassert_equal(net.connect_calls, 9, "never more than a minute apart");
	zassert_mem_equal(net.password, wifi_password, sizeof(wifi_password) - 1,
			  "with the stored password");

	/* It comes back and joins; a later drop starts from five seconds again. */
	net.wifi.wifi_associates = true;
	advance_seconds(61);
	(void)network_manager_process();
	zassert_true(net.associated);
	(void)network_manager_process();

	net.associated = false;
	net.wifi.link_up = false;
	advance_seconds(4);
	(void)network_manager_process();
	zassert_equal(net.connect_calls, 10);
	advance_seconds(2);
	(void)network_manager_process();
	zassert_equal(net.connect_calls, 11, "the backoff started over");
}

static const char *apply_during_scan_id;

static void apply_during_scan(void *arg)
{
	struct api_error err;
	char id[JOB_ID_MAX_LEN + 1];

	ARG_UNUSED(arg);
	zassert_ok(network_apply(apply_during_scan_id, 0, NULL, 0, &err, id),
		   "an apply is accepted while the worker scans");
}

/*
 * No rejoin while a change is about to reach the interfaces: the push joins
 * the candidate's network itself, and a rejoin of the committed one in between
 * would spend an association on a network that is being replaced. An apply
 * accepted while the worker is busy with a scan is exactly that moment. Found
 * by mutation: dropping the applying/rolling_back guard broke nothing, because
 * no test reached the policy with a change accepted but not yet pushed.
 */
ZTEST(network_manager, test_no_rejoin_while_an_accepted_change_waits_for_its_push)
{
	struct network_config_input in = wifi_input();
	struct network_transaction txn;
	struct api_error err;
	char scan_job[JOB_ID_MAX_LEN + 1];

	net.wifi.link_up = true;
	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);

	/* The AP goes away, and the first retry is due. */
	net.associated = false;
	net.wifi.link_up = false;
	net.wifi.wifi_associates = false;
	advance_seconds(6);

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_scan_begin(NULL, 0, &err, scan_job));
	apply_during_scan_id = txn.id;
	net.during_io = apply_during_scan;
	unsigned int calls = net.connect_calls;

	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_APPLYING, "accepted during the scan");
	zassert_equal(net.connect_calls, calls, "the committed network is not rejoined now");

	zassert_true(network_manager_process() > 0);
	zassert_equal(net.connect_calls, calls + 1U, "the push joins, once");
}

ZTEST(network_manager, test_no_rejoin_while_connecting_disabled_or_absent)
{
	struct network_config_input in = wifi_input();
	struct network_transaction txn;

	net.wifi.link_up = true;
	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);

	net.associated = false;
	net.connecting = true;
	advance_seconds(30);
	(void)network_manager_process();
	zassert_equal(net.connect_calls, 1, "an attempt still in progress is not repeated");

	net.connecting = false;
	net.wifi.present = false;
	advance_seconds(30);
	(void)network_manager_process();
	zassert_equal(net.connect_calls, 1, "a radio that is not there is not asked");

	/* A factory device, Ethernet only: nothing to rejoin. */
	fake_storage_init(&storage);
	fake_storage_bind(&storage, &store_backend);
	zassert_ok(device_config_init(&store_backend, NULL));
	fake_net_init(&net);
	bring_up();
	advance_seconds(30);
	(void)network_manager_process();
	zassert_equal(net.connect_calls, 0);
}

/*
 * While a change awaits confirmation, the network being rejoined is the
 * candidate's, and so is its password: the pending generation, not the
 * committed one. Found by mutation: rejoining with the committed password broke
 * nothing, because every rejoin so far happened after a commit.
 */
ZTEST(network_manager, test_a_rejoin_while_awaiting_confirmation_uses_the_new_password)
{
	static const uint8_t new_password[] = "a-new-and-better-one";
	struct network_config_input in = wifi_input();
	struct network_transaction txn;

	net.wifi.link_up = true;
	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);

	struct network_config_input changed = wifi_input();

	changed.wifi_password.value = new_password;
	changed.wifi_password.len = sizeof(new_password) - 1;
	apply_to_awaiting(&changed, &txn);

	/* The AP drops the device before anyone confirmed. */
	net.associated = false;
	net.wifi.link_up = false;
	net.wifi.wifi_associates = false;
	advance_seconds(6);
	unsigned int calls = net.connect_calls;

	(void)network_manager_process();
	zassert_equal(net.connect_calls, calls + 1U, "the dropped network is joined again");
	zassert_equal(net.password_len, sizeof(new_password) - 1);
	zassert_mem_equal(net.password, new_password, net.password_len,
			  "with the password of the change being tried, not the committed one");
}

/* --- scan --------------------------------------------------------------- */

ZTEST(network_manager, test_scan_runs_on_the_worker_and_returns_results)
{
	struct api_error err;
	struct network_scan_results results;
	struct job_snapshot snapshot;

	net.scan_results.count = 3;
	net.scan_results.items[0].rssi = -40;
	net.scan_results.items[0].ssid_len = 5;
	memcpy(net.scan_results.items[0].ssid, "cedar", 5);
	net.scan_results.items[0].security = NETWORK_AP_WPA3_SAE;
	net.scan_results.items[1].security = NETWORK_AP_ENTERPRISE;
	net.scan_results.items[1].connect_supported = true;
	net.scan_results.items[2].security = (enum network_ap_security)42;

	zassert_ok(network_scan_begin(NULL, 0, &err, job));
	zassert_equal(network_scan_results_get(job, &results), -EAGAIN);
	zassert_equal(job_state_of(job), JOB_STATE_QUEUED);
	zassert_equal(net.scan_calls, 0, "the handler does not scan");

	zassert_true(network_manager_process() > 0);
	zassert_ok(network_scan_results_get(job, &results));
	zassert_equal(results.count, 3);
	zassert_false(results.truncated);
	zassert_equal(results.items[0].rssi, -40);
	zassert_true(results.items[0].connect_supported);
	zassert_false(results.items[1].connect_supported,
		      "an adapter cannot offer a network this build cannot join");
	zassert_equal(results.items[2].security, NETWORK_AP_UNKNOWN);
	zassert_false(results.items[2].connect_supported);
	zassert_ok(job_get(job, &snapshot));
	zassert_equal(snapshot.state, JOB_STATE_SUCCEEDED);
}

/*
 * An adapter is trusted to fill the array but not to count it: a count past
 * the end would be read as valid entries by everything downstream.
 */
ZTEST(network_manager, test_scan_results_are_capped_and_the_client_told)
{
	struct api_error err;
	struct network_scan_results results;

	net.scan_results.count = 255;

	zassert_ok(network_scan_begin(NULL, 0, &err, job));
	zassert_true(network_manager_process() > 0);
	zassert_ok(network_scan_results_get(job, &results));
	zassert_equal(results.count, CONFIG_NETWORK_MANAGER_SCAN_MAX_RESULTS);
	zassert_true(results.truncated, "a partial list must not look complete");
}

ZTEST(network_manager, test_a_failed_scan_fails_its_job)
{
	struct api_error err;
	struct network_scan_results results;
	struct job_snapshot snapshot;

	net.fail_scan = -ETIMEDOUT;
	zassert_ok(network_scan_begin(NULL, 0, &err, job));
	zassert_true(network_manager_process() > 0);
	zassert_equal(network_scan_results_get(job, &results), -EIO);
	zassert_ok(job_get(job, &snapshot));
	zassert_equal(snapshot.state, JOB_STATE_FAILED);
	zassert_str_equal(snapshot.error.code, "service_not_ready");
}

ZTEST(network_manager, test_only_the_latest_scan_keeps_its_results)
{
	struct api_error err;
	struct network_scan_results results;
	char first[JOB_ID_MAX_LEN + 1];

	zassert_ok(network_scan_begin(NULL, 0, &err, job));
	memcpy(first, job, sizeof(first));
	zassert_true(network_manager_process() > 0);
	zassert_ok(network_scan_begin(NULL, 0, &err, job));
	zassert_true(network_manager_process() > 0);

	zassert_equal(network_scan_results_get(first, &results), -ENOENT);
	zassert_ok(network_scan_results_get(job, &results));
}

ZTEST(network_manager, test_scan_is_refused_during_a_change_and_while_one_runs)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	char scan_job[JOB_ID_MAX_LEN + 1];

	/* A staged candidate has changed nothing, so scanning is still fine. */
	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_scan_begin(NULL, 0, &err, scan_job));

	zassert_equal(network_scan_begin(NULL, 0, &err, scan_job), -EBUSY,
		      "one scan at a time");
	zassert_equal(err.code, API_ERR_BUSY);
	zassert_true(network_manager_process() > 0);

	zassert_ok(apply(txn.id, 0, &err));
	zassert_equal(network_scan_begin(NULL, 0, &err, scan_job), -EBUSY,
		      "a scan takes the radio off the channel it is associated on");
	zassert_equal(err.code, API_ERR_BUSY);

	zassert_true(network_manager_process() > 0);
	zassert_equal(network_scan_begin(NULL, 0, &err, scan_job), -EBUSY);

	confirm_and_commit(&txn);
	zassert_ok(network_scan_begin(NULL, 0, &err, scan_job));
}

/*
 * Rolling back is still the change: the committed configuration is being put
 * back on the radio, and a scan would take it off its channel mid-restore. The
 * gap this fills was found by mutation: dropping rolling_back from the busy
 * check broke no test, because none scanned between a rollback and the worker.
 */
ZTEST(network_manager, test_scan_is_refused_while_a_change_rolls_back)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	char scan_job[JOB_ID_MAX_LEN + 1];

	apply_to_awaiting(&in, &txn);
	zassert_ok(rollback(txn.id, &err));
	zassert_equal(state_of(txn.id), NETWORK_TXN_ROLLING_BACK);

	zassert_equal(network_scan_begin(NULL, 0, &err, scan_job), -EBUSY,
		      "the committed configuration is being restored on that radio");
	zassert_equal(err.code, API_ERR_BUSY);

	zassert_true(network_manager_process() > 0);
	zassert_equal(state_of(txn.id), NETWORK_TXN_ROLLED_BACK);
	zassert_ok(network_scan_begin(NULL, 0, &err, scan_job), "a finished rollback frees the radio");
}

ZTEST(network_manager, test_a_scan_queued_before_an_apply_does_not_run_during_it)
{
	struct network_config_input in = valid_input();
	struct api_error err;
	struct network_transaction txn;
	char scan_job[JOB_ID_MAX_LEN + 1];

	zassert_ok(stage(&in, &err, &txn));
	zassert_ok(network_scan_begin(NULL, 0, &err, scan_job));
	zassert_ok(apply(txn.id, 0, &err));

	zassert_true(network_manager_process() > 0);
	zassert_equal(net.scan_calls, 0);
	zassert_equal(job_state_of(scan_job), JOB_STATE_FAILED);
}

ZTEST(network_manager, test_repeating_a_scan_request_returns_the_same_job)
{
	struct api_error err;
	char first[JOB_ID_MAX_LEN + 1];
	char second[JOB_ID_MAX_LEN + 1];

	zassert_ok(network_scan_begin("idem-key-0123456789", 7, &err, first));
	zassert_ok(network_scan_begin("idem-key-0123456789", 7, &err, second),
		   "a replay is not refused as busy");
	zassert_str_equal(first, second, "a retry must not start a second scan");

	zassert_equal(network_scan_begin("idem-key-0123456789", 99, &err, second), -EINVAL);
	zassert_equal(err.code, API_ERR_IDEMPOTENCY_CONFLICT);
}

ZTEST(network_manager, test_scan_without_a_radio_is_unavailable)
{
	struct api_error err;

	net.no_radio = true;
	bring_up();

	zassert_equal(network_scan_begin(NULL, 0, &err, job), -ENOTSUP);
	zassert_equal(err.code, API_ERR_CAPABILITY_UNAVAILABLE);
}

ZTEST(network_manager, test_scan_with_an_absent_coprocessor_is_unavailable)
{
	struct api_error err;

	net.wifi.present = false;
	zassert_equal(network_scan_begin(NULL, 0, &err, job), -ENOTSUP);
	zassert_equal(err.code, API_ERR_CAPABILITY_UNAVAILABLE);
	zassert_equal(net.scan_calls, 0);
}

/* --- boot and the physical recovery ------------------------------------- */

ZTEST(network_manager, test_boot_puts_the_committed_configuration_on_the_interfaces)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;
	struct device_config_recovery_report report;

	apply_to_awaiting(&in, &txn);
	confirm_and_commit(&txn);

	/* Reboot. */
	fake_net_init(&net);
	zassert_ok(device_config_init(&store_backend, &report));
	bring_up();
	zassert_equal(net.eth.configure_calls, 0);

	network_manager_boot(&report);
	zassert_true(network_manager_process() > 0);
	zassert_equal(net.eth.configure_calls, 1);
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_STATIC);
	zassert_equal(net.eth.applied.address[3], 14);
	zassert_equal(net.set_dns_calls, 1);
	zassert_equal(net.disconnect_calls, 1, "Wi-Fi is off in that configuration");

	(void)network_manager_process();
	zassert_equal(net.eth.configure_calls, 1, "once");
}

ZTEST(network_manager, test_a_boot_that_rolled_back_remembers_the_transaction)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;
	struct device_config_recovery_report report;

	apply_to_awaiting(&in, &txn);

	/* Power lost while awaiting confirmation. */
	fake_net_init(&net);
	zassert_ok(device_config_init(&store_backend, &report));
	zassert_equal(report.result, DEVICE_CONFIG_RECOVERY_ROLLED_BACK);
	bring_up();
	network_manager_boot(&report);

	zassert_true(network_manager_lost_in_reboot(txn.id));
	zassert_false(network_manager_lost_in_reboot("txn_other"));
	zassert_equal(network_transaction_get(txn.id, &txn), -ENOENT);

	zassert_true(network_manager_process() > 0);
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP,
		      "the committed default is what comes up");
}

ZTEST(network_manager, test_restoring_defaults_keeps_the_profile_and_drops_the_password)
{
	const uint8_t verifier[] = {0xA5, 0x5A, 0x01, 0x02};
	struct device_config_update seed;
	struct device_config cfg;
	uint8_t buf[DEVICE_CONFIG_SECRET_MAX_LEN];
	size_t len = 1;

	/* A configured device: static Ethernet, manual DNS, Wi-Fi with a password, an admin. */
	memset(&seed, 0, sizeof(seed));
	seed.config = wifi_input().config;
	seed.config.dns.mode = DEVICE_CONFIG_DNS_MANUAL;
	seed.config.dns.server_count = 1;
	seed.config.dns.servers[0].family = DEVICE_CONFIG_AF_INET;
	seed.config.dns.servers[0].bytes[0] = 1;
	seed.config.wifi.hidden = 1;
	seed.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].action = DEVICE_CONFIG_SECRET_REPLACE;
	seed.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].value = wifi_password;
	seed.secrets[DEVICE_CONFIG_SECRET_WIFI_PASSWORD].len = sizeof(wifi_password) - 1;
	seed.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].action = DEVICE_CONFIG_SECRET_REPLACE;
	seed.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].value = verifier;
	seed.secrets[DEVICE_CONFIG_SECRET_ADMIN_PASSWORD].len = sizeof(verifier);
	zassert_ok(device_config_pending_begin(&seed, 0, "seed"));
	zassert_ok(device_config_pending_commit("seed"));

	zassert_ok(network_manager_restore_defaults());
	zassert_equal(device_config_revision(), 2);

	zassert_ok(device_config_get(DEVICE_CONFIG_COMMITTED, &cfg));
	zassert_true(cfg.ethernet.enabled);
	zassert_equal(cfg.ethernet.ipv4.mode, DEVICE_CONFIG_IPV4_DHCP);
	zassert_equal(cfg.dns.mode, DEVICE_CONFIG_DNS_AUTOMATIC);
	zassert_equal(cfg.dns.server_count, 0);
	zassert_equal(cfg.preferred_interface, DEVICE_CONFIG_INTERFACE_ETHERNET);
	zassert_false(cfg.wifi.enabled);
	zassert_equal(cfg.wifi.ssid_len, 5, "the form still says which network it was");
	zassert_mem_equal(cfg.wifi.ssid, "cedar", 5);
	zassert_equal(cfg.wifi.security, DEVICE_CONFIG_WIFI_WPA3_SAE);
	zassert_true(cfg.wifi.hidden);
	zassert_false(cfg.wifi.password_set);

	zassert_ok(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					    DEVICE_CONFIG_SECRET_WIFI_PASSWORD, buf, sizeof(buf),
					    &len));
	zassert_equal(len, 0);
	zassert_ok(device_config_secret_get(DEVICE_CONFIG_COMMITTED,
					    DEVICE_CONFIG_SECRET_ADMIN_PASSWORD, buf, sizeof(buf),
					    &len));
	zassert_equal(len, sizeof(verifier), "the administrator is not network configuration");

	zassert_true(network_manager_process() > 0);
	zassert_equal(net.eth.applied.mode, DEVICE_CONFIG_IPV4_DHCP, "pushed on the next pass");
}

ZTEST(network_manager, test_restoring_defaults_waits_for_a_transaction)
{
	struct network_config_input in = valid_input();
	struct network_transaction txn;

	apply_to_awaiting(&in, &txn);
	zassert_equal(network_manager_restore_defaults(), -EBUSY);
	zassert_equal(device_config_revision(), 0);
}

/* --- wire names --------------------------------------------------------- */

/* These strings are the HTTP contract; a typo here is a broken API. */
ZTEST(network_manager, test_state_names_match_the_contract)
{
	zassert_str_equal(network_transaction_state_str(NETWORK_TXN_STAGED), "staged");
	zassert_str_equal(network_transaction_state_str(NETWORK_TXN_APPLYING), "applying");
	zassert_str_equal(network_transaction_state_str(NETWORK_TXN_AWAITING_CONFIRMATION),
			  "awaiting_confirmation");
	zassert_str_equal(network_transaction_state_str(NETWORK_TXN_COMMITTED), "committed");
	zassert_str_equal(network_transaction_state_str(NETWORK_TXN_ROLLING_BACK), "rolling_back");
	zassert_str_equal(network_transaction_state_str(NETWORK_TXN_ROLLED_BACK), "rolled_back");
	zassert_str_equal(network_transaction_state_str(NETWORK_TXN_FAILED), "failed");
	zassert_str_equal(network_transaction_state_str(NETWORK_TXN_EXPIRED), "expired");

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

ZTEST(network_manager, test_status_and_scan_names_match_the_contract)
{
	const char *const iface[] = {"disabled", "down",     "connecting", "connected",
				     "addressing", "ready", "failed"};
	const char *const security[] = {"open",       "wpa2_psk",   "wpa3_sae",
					"wpa2_wpa3_transition", "enterprise", "unknown"};
	const char *const source[] = {"dhcp", "static", "slaac", "link_local"};

	zassert_equal(ARRAY_SIZE(iface), NETWORK_IFACE_STATE_COUNT);
	for (int i = 0; i < NETWORK_IFACE_STATE_COUNT; i++) {
		zassert_str_equal(network_iface_state_str(i), iface[i]);
	}
	zassert_is_null(network_iface_state_str(NETWORK_IFACE_STATE_COUNT));

	zassert_equal(ARRAY_SIZE(security), NETWORK_AP_SECURITY_COUNT);
	for (int i = 0; i < NETWORK_AP_SECURITY_COUNT; i++) {
		zassert_str_equal(network_ap_security_str(i), security[i]);
	}
	zassert_is_null(network_ap_security_str(NETWORK_AP_SECURITY_COUNT));

	zassert_equal(ARRAY_SIZE(source), NETWORK_ADDR_SOURCE_COUNT);
	for (int i = 0; i < NETWORK_ADDR_SOURCE_COUNT; i++) {
		zassert_str_equal(network_addr_source_str(i), source[i]);
	}
	zassert_is_null(network_addr_source_str(NETWORK_ADDR_SOURCE_COUNT));

	zassert_true(network_ap_security_connectable(NETWORK_AP_OPEN));
	zassert_true(network_ap_security_connectable(NETWORK_AP_WPA2_PSK));
	zassert_true(network_ap_security_connectable(NETWORK_AP_WPA3_SAE));
	zassert_true(network_ap_security_connectable(NETWORK_AP_WPA2_WPA3_TRANSITION));
	zassert_false(network_ap_security_connectable(NETWORK_AP_ENTERPRISE));
	zassert_false(network_ap_security_connectable(NETWORK_AP_UNKNOWN));
}

ZTEST(network_manager, test_init_rejects_an_incomplete_adapter)
{
	struct network_iface_ops broken;

	fake_net_bind(&net, &broken);
	broken.configure = NULL;
	zassert_equal(network_manager_init(&broken), -EINVAL);
	zassert_equal(network_manager_init(NULL), -EINVAL);

	/* The optional calls are optional. */
	net.no_get_dns = true;
	net.no_set_default = true;
	net.no_radio = true;
	fake_net_bind(&net, &broken);
	zassert_ok(network_manager_init(&broken));
}
