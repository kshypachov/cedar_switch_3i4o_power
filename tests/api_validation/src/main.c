/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API validation unit tests.
 *
 * Two things are being protected here. One is the error table: the codes, the
 * statuses they map to and the retryable flags are the HTTP contract, and a
 * typo in any of them is a broken API that still compiles. The other is the
 * parsers, which are deliberately strict, and where the interesting cases are
 * all rejections.
 *
 * The field list is configured to two entries in prj.conf so that truncation
 * is exercised rather than assumed.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <api_validation/api_validation.h>

static void case_before(void *fixture)
{
	ARG_UNUSED(fixture);
	/* Deterministic ids: the tests assert on exact JSON. */
	api_request_id_seed(0x1000);
}

ZTEST_SUITE(api_validation, NULL, NULL, case_before, NULL, NULL);

/* --- the error table ---------------------------------------------------- */

/*
 * Transcribed from the table in api-contract.md. This test is the transcription
 * being checked against itself: if the table in the contract changes, this
 * fails and someone has to look at both.
 */
ZTEST(api_validation, test_codes_map_to_the_documented_statuses)
{
	const struct {
		enum api_error_code code;
		uint16_t status;
		const char *name;
	} expected[] = {
		{API_ERR_INVALID_JSON, 400, "invalid_json"},
		{API_ERR_INVALID_QUERY, 400, "invalid_query"},
		{API_ERR_INVALID_CURSOR, 400, "invalid_cursor"},
		{API_ERR_AUTHENTICATION_REQUIRED, 401, "authentication_required"},
		{API_ERR_INVALID_CREDENTIALS, 401, "invalid_credentials"},
		{API_ERR_SESSION_EXPIRED, 401, "session_expired"},
		{API_ERR_CSRF_FAILED, 403, "csrf_failed"},
		{API_ERR_ORIGIN_REJECTED, 403, "origin_rejected"},
		{API_ERR_SETUP_NOT_ALLOWED, 403, "setup_not_allowed"},
		{API_ERR_NOT_FOUND, 404, "not_found"},
		{API_ERR_BUSY, 409, "busy"},
		{API_ERR_STALE_REVISION, 409, "stale_revision"},
		{API_ERR_INVALID_STATE, 409, "invalid_state"},
		{API_ERR_OFFSET_MISMATCH, 409, "offset_mismatch"},
		{API_ERR_IDEMPOTENCY_CONFLICT, 409, "idempotency_conflict"},
		{API_ERR_ETHERNET_REQUIRED, 409, "ethernet_required"},
		{API_ERR_RESOURCE_EXPIRED, 410, "resource_expired"},
		{API_ERR_BOOT_CHANGED, 410, "boot_changed"},
		{API_ERR_PAYLOAD_TOO_LARGE, 413, "payload_too_large"},
		{API_ERR_UNSUPPORTED_MEDIA_TYPE, 415, "unsupported_media_type"},
		{API_ERR_VALIDATION_FAILED, 422, "validation_failed"},
		{API_ERR_INVALID_IMAGE, 422, "invalid_image"},
		{API_ERR_UNSUPPORTED_TARGET, 422, "unsupported_target"},
		{API_ERR_INCOMPATIBLE_FIRMWARE, 422, "incompatible_firmware"},
		{API_ERR_SIGNATURE_INVALID, 422, "signature_invalid"},
		{API_ERR_RATE_LIMITED, 429, "rate_limited"},
		{API_ERR_INTERNAL_ERROR, 500, "internal_error"},
		{API_ERR_SERVICE_NOT_READY, 503, "service_not_ready"},
		{API_ERR_CAPABILITY_UNAVAILABLE, 503, "capability_unavailable"},
		{API_ERR_STORAGE_FULL, 507, "storage_full"},
	};

	zassert_equal(ARRAY_SIZE(expected), API_ERR_COUNT,
		      "a code was added without a row in this table");

	for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
		zassert_equal(api_error_status(expected[i].code), expected[i].status,
			      "%s has the wrong status", expected[i].name);
		zassert_equal(strcmp(api_error_str(expected[i].code), expected[i].name), 0,
			      "wire name %zu", i);
	}
}

ZTEST(api_validation, test_every_code_has_a_name_and_a_status)
{
	for (int c = 0; c < API_ERR_COUNT; c++) {
		zassert_not_null(api_error_str(c), "code %d has no wire name", c);
		zassert_true(api_error_status(c) >= 400, "code %d has a non-error status", c);
	}
	zassert_is_null(api_error_str(API_ERR_COUNT));
	zassert_is_null(api_error_str(-1));

	/* An unknown code must not be reported as something the client can act
	 * on; 500 is the only honest answer.
	 */
	zassert_equal(api_error_status(API_ERR_COUNT), 500);
	zassert_false(api_error_is_retryable(API_ERR_COUNT));
}

/*
 * retryable is a promise, so it is asserted rather than left to whoever edits
 * the table next. The division is: a conflict of timing may clear, a conflict
 * of content or of authorisation never does.
 */
ZTEST(api_validation, test_retryable_follows_the_code)
{
	zassert_true(api_error_is_retryable(API_ERR_BUSY));
	zassert_true(api_error_is_retryable(API_ERR_RATE_LIMITED));
	zassert_true(api_error_is_retryable(API_ERR_SERVICE_NOT_READY));
	zassert_true(api_error_is_retryable(API_ERR_SESSION_EXPIRED));
	zassert_true(api_error_is_retryable(API_ERR_INTERNAL_ERROR));

	zassert_false(api_error_is_retryable(API_ERR_VALIDATION_FAILED));
	zassert_false(api_error_is_retryable(API_ERR_STALE_REVISION));
	zassert_false(api_error_is_retryable(API_ERR_IDEMPOTENCY_CONFLICT));
	zassert_false(api_error_is_retryable(API_ERR_CSRF_FAILED));
	zassert_false(api_error_is_retryable(API_ERR_INVALID_CREDENTIALS));
	zassert_false(api_error_is_retryable(API_ERR_STORAGE_FULL));
	zassert_false(api_error_is_retryable(API_ERR_CAPABILITY_UNAVAILABLE),
		      "a capability the build does not have will not appear by waiting");
}

ZTEST(api_validation, test_field_codes_have_wire_names)
{
	zassert_equal(strcmp(api_field_code_str(API_FIELD_REQUIRED), "required"), 0);
	zassert_equal(strcmp(api_field_code_str(API_FIELD_INVALID_FORMAT), "invalid_format"), 0);
	zassert_equal(strcmp(api_field_code_str(API_FIELD_UNKNOWN), "unknown_field"), 0);

	for (int c = 0; c < API_FIELD_CODE_COUNT; c++) {
		zassert_not_null(api_field_code_str(c), "field code %d has no name", c);
	}
	zassert_is_null(api_field_code_str(API_FIELD_CODE_COUNT));
}

/* --- building and rendering --------------------------------------------- */

/* The exact body from the contract's ErrorDetail example. */
ZTEST(api_validation, test_renders_the_contract_example)
{
	struct api_error err;
	char buf[512];

	zassert_ok(api_error_init(&err, API_ERR_VALIDATION_FAILED,
				  "Static IPv4 requires an address and prefix", "req_7b22"));
	zassert_ok(api_error_add_field(&err, "/interfaces/ethernet/ipv4/address",
				       API_FIELD_REQUIRED));

	int n = api_error_to_json(&err, buf, sizeof(buf));

	zassert_true(n > 0, "render failed: %d", n);
	zassert_equal(strcmp(buf,
			     "{\"error\":{\"code\":\"validation_failed\","
			     "\"message\":\"Static IPv4 requires an address and prefix\","
			     "\"request_id\":\"req_7b22\",\"retryable\":false,"
			     "\"fields\":[{\"path\":\"/interfaces/ethernet/ipv4/address\","
			     "\"code\":\"required\"}]}}"),
		      0, "got: %s", buf);
	zassert_equal((size_t)n, strlen(buf));
}

ZTEST(api_validation, test_fields_are_omitted_when_empty)
{
	struct api_error err;
	char buf[256];

	zassert_ok(api_error_init(&err, API_ERR_BUSY, "Another operation is running", "req_1"));
	zassert_true(api_error_to_json(&err, buf, sizeof(buf)) > 0);

	zassert_is_null(strstr(buf, "fields"),
			"an empty array would read as 'the fields are fine'");
	zassert_not_null(strstr(buf, "\"retryable\":true"));
}

ZTEST(api_validation, test_strings_are_escaped)
{
	struct api_error err;
	char buf[512];

	zassert_ok(api_error_init(&err, API_ERR_INVALID_JSON,
				  "Unexpected \"token\" at line 1\nnear \\x", "req_1"));
	zassert_ok(api_error_add_field(&err, "/a\"b", API_FIELD_INVALID_FORMAT));
	zassert_true(api_error_to_json(&err, buf, sizeof(buf)) > 0);

	zassert_not_null(strstr(buf, "\\\"token\\\""), "quotes must be escaped: %s", buf);
	zassert_not_null(strstr(buf, "\\n"));
	zassert_not_null(strstr(buf, "\\\\x"));
	zassert_not_null(strstr(buf, "/a\\\"b"));

	/* The document must still be balanced after all that. */
	zassert_equal(buf[strlen(buf) - 1], '}');
}

/*
 * A body cut in half is worse than no body: the client parses it, fails, and
 * cannot tell a truncated error from a malformed server.
 */
ZTEST(api_validation, test_short_buffer_yields_nothing_rather_than_half)
{
	struct api_error err;
	char buf[32];

	zassert_ok(api_error_init(&err, API_ERR_VALIDATION_FAILED,
				  "A message far longer than the buffer allows", "req_1"));
	zassert_equal(api_error_to_json(&err, buf, sizeof(buf)), -ENOMEM);
	zassert_equal(api_error_to_json(&err, buf, 0), -ENOMEM);
	zassert_equal(api_error_to_json(&err, NULL, sizeof(buf)), -ENOMEM);
}

ZTEST(api_validation, test_message_is_truncated_not_overflowed)
{
	struct api_error err;
	char message[API_ERROR_MESSAGE_MAX_LEN + 64];

	memset(message, 'x', sizeof(message) - 1);
	message[sizeof(message) - 1] = '\0';

	zassert_ok(api_error_init(&err, API_ERR_INTERNAL_ERROR, message, "req_1"));
	zassert_equal(strlen(err.message), API_ERROR_MESSAGE_MAX_LEN);
}

ZTEST(api_validation, test_field_list_truncation_is_recorded)
{
	struct api_error err;
	char buf[512];

	/* prj.conf sets the limit to two. */
	zassert_ok(api_error_init(&err, API_ERR_VALIDATION_FAILED, "Several problems", "req_1"));
	zassert_ok(api_error_add_field(&err, "/a", API_FIELD_REQUIRED));
	zassert_ok(api_error_add_field(&err, "/b", API_FIELD_REQUIRED));
	zassert_equal(api_error_add_field(&err, "/c", API_FIELD_REQUIRED), -ENOSPC);

	zassert_true(err.fields_truncated,
		     "a client shown two of three problems must be told there are more");
	zassert_equal(err.field_count, 2);

	zassert_true(api_error_to_json(&err, buf, sizeof(buf)) > 0);
	zassert_not_null(strstr(buf, "\"/a\""));
	zassert_not_null(strstr(buf, "\"/b\""));
	zassert_is_null(strstr(buf, "\"/c\""));

	/* ErrorDetail is additionalProperties false, so the message is the only
	 * place the client can be told the list is partial.
	 */
	zassert_not_null(strstr(buf, "Several problems (further problems were not reported)"),
			 "got: %s", buf);
}

ZTEST(api_validation, test_init_and_add_field_reject_bad_arguments)
{
	struct api_error err;

	zassert_equal(api_error_init(NULL, API_ERR_BUSY, "m", "req_1"), -EINVAL);
	zassert_equal(api_error_init(&err, API_ERR_COUNT, "m", "req_1"), -EINVAL);
	zassert_equal(api_error_init(&err, API_ERR_BUSY, "m", "not a valid id"), -EINVAL,
		      "a request id is echoed in a header and must match the pattern");

	zassert_ok(api_error_init(&err, API_ERR_BUSY, "m", "req_1"));
	zassert_equal(api_error_add_field(&err, "no-leading-slash", API_FIELD_REQUIRED), -EINVAL);
	zassert_equal(api_error_add_field(&err, NULL, API_FIELD_REQUIRED), -EINVAL);
	zassert_equal(api_error_add_field(&err, "/a", API_FIELD_CODE_COUNT), -EINVAL);
	zassert_equal(api_error_add_field(NULL, "/a", API_FIELD_REQUIRED), -EINVAL);

	/* A path that would not fit is refused rather than silently shortened:
	 * half a JSON Pointer points somewhere else.
	 */
	char long_path[API_ERROR_PATH_MAX_LEN + 8];

	long_path[0] = '/';
	memset(&long_path[1], 'a', sizeof(long_path) - 2);
	long_path[sizeof(long_path) - 1] = '\0';
	zassert_equal(api_error_add_field(&err, long_path, API_FIELD_REQUIRED), -EINVAL);
}

ZTEST(api_validation, test_null_message_is_rendered_as_empty)
{
	struct api_error err;
	char buf[256];

	zassert_ok(api_error_init(&err, API_ERR_INTERNAL_ERROR, NULL, "req_1"));
	zassert_true(api_error_to_json(&err, buf, sizeof(buf)) > 0);
	zassert_not_null(strstr(buf, "\"message\":\"\""));
}

/* --- request ids -------------------------------------------------------- */

ZTEST(api_validation, test_request_ids_are_unique_and_well_formed)
{
	char a[API_REQUEST_ID_MAX_LEN + 1];
	char b[API_REQUEST_ID_MAX_LEN + 1];

	zassert_ok(api_request_id_generate(a, sizeof(a)));
	zassert_ok(api_request_id_generate(b, sizeof(b)));

	zassert_not_equal(strcmp(a, b), 0, "ids must not repeat within a boot");
	zassert_true(api_validate_opaque_id(a), "'%s' must match the id pattern", a);
	zassert_equal(strncmp(a, "req_", 4), 0);
	zassert_equal(strlen(a), 12);

	zassert_equal(api_request_id_generate(a, 12), -ENOMEM);
	zassert_equal(api_request_id_generate(NULL, sizeof(a)), -ENOMEM);
}

ZTEST(api_validation, test_init_generates_an_id_when_none_is_given)
{
	struct api_error err;

	zassert_ok(api_error_init(&err, API_ERR_NOT_FOUND, "No such job", NULL));
	zassert_true(api_validate_opaque_id(err.request_id));
}

/* --- validators --------------------------------------------------------- */

ZTEST(api_validation, test_idempotency_key_bounds)
{
	char key[API_IDEMPOTENCY_KEY_MAX_LEN + 8];

	memset(key, 'k', sizeof(key) - 1);
	key[sizeof(key) - 1] = '\0';

	/* 15 is one short, 16 and 64 are the documented ends. */
	key[API_IDEMPOTENCY_KEY_MIN_LEN - 1] = '\0';
	zassert_false(api_validate_idempotency_key(key));

	key[API_IDEMPOTENCY_KEY_MIN_LEN - 1] = 'k';
	key[API_IDEMPOTENCY_KEY_MIN_LEN] = '\0';
	zassert_true(api_validate_idempotency_key(key));

	memset(key, 'k', sizeof(key) - 1);
	key[API_IDEMPOTENCY_KEY_MAX_LEN] = '\0';
	zassert_true(api_validate_idempotency_key(key));

	key[API_IDEMPOTENCY_KEY_MAX_LEN] = 'k';
	key[API_IDEMPOTENCY_KEY_MAX_LEN + 1] = '\0';
	zassert_false(api_validate_idempotency_key(key), "65 characters is over the limit");

	zassert_false(api_validate_idempotency_key(NULL));
	zassert_false(api_validate_idempotency_key(""));
	zassert_false(api_validate_idempotency_key("has a space in it and is long"),
		      "the value is logged and used as a map key");
	zassert_false(api_validate_idempotency_key("has\na\nnewline\nin\nit\nsomewhere"));
}

ZTEST(api_validation, test_opaque_id_pattern)
{
	zassert_true(api_validate_opaque_id("job_f0d2"));
	zassert_true(api_validate_opaque_id("a"));
	zassert_true(api_validate_opaque_id("A-Z_0-9"));

	zassert_false(api_validate_opaque_id(""));
	zassert_false(api_validate_opaque_id(NULL));
	zassert_false(api_validate_opaque_id("has/slash"), "an id goes straight into a URL");
	zassert_false(api_validate_opaque_id("has.dot"));
	zassert_false(api_validate_opaque_id("has space"));

	char too_long[API_REQUEST_ID_MAX_LEN + 2];

	memset(too_long, 'a', sizeof(too_long) - 1);
	too_long[sizeof(too_long) - 1] = '\0';
	zassert_false(api_validate_opaque_id(too_long));
}

ZTEST(api_validation, test_ipv4_parsing_accepts_valid_addresses)
{
	uint8_t out[4];

	zassert_ok(api_parse_ipv4("192.168.88.14", out));
	zassert_equal(out[0], 192);
	zassert_equal(out[1], 168);
	zassert_equal(out[2], 88);
	zassert_equal(out[3], 14);

	zassert_ok(api_parse_ipv4("0.0.0.0", out));
	zassert_ok(api_parse_ipv4("255.255.255.255", out));
	zassert_equal(out[3], 255);
}

ZTEST(api_validation, test_ipv4_parsing_is_strict)
{
	uint8_t out[4];

	zassert_equal(api_parse_ipv4("010.0.0.1", out), -EINVAL,
		      "a leading zero reads as octal to some parsers and decimal to others");
	zassert_equal(api_parse_ipv4("192.168.88", out), -EINVAL);
	zassert_equal(api_parse_ipv4("192.168.88.14.5", out), -EINVAL);
	zassert_equal(api_parse_ipv4("192.168.88.256", out), -EINVAL);
	zassert_equal(api_parse_ipv4("192.168.88.1 ", out), -EINVAL, "no trailing whitespace");
	zassert_equal(api_parse_ipv4(" 192.168.88.1", out), -EINVAL);
	zassert_equal(api_parse_ipv4("192.168.88.1/24", out), -EINVAL);
	zassert_equal(api_parse_ipv4("192.168..1", out), -EINVAL);
	zassert_equal(api_parse_ipv4("192.168.88.0001", out), -EINVAL);
	zassert_equal(api_parse_ipv4("", out), -EINVAL);
	zassert_equal(api_parse_ipv4(NULL, out), -EINVAL);
	zassert_equal(api_parse_ipv4("192.168.88.1", NULL), -EINVAL);
}

ZTEST(api_validation, test_ipv6_parsing)
{
	uint8_t out[16];
	const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
	const uint8_t link_local[16] = {0xfe, 0x80, 0, 0, 0,    0,    0,    0,
					0x82, 0x34, 0x28, 0xff, 0xfe, 0x10, 0x12, 0x73};

	zassert_ok(api_parse_ipv6("::1", out));
	zassert_mem_equal(out, loopback, 16);

	/* The board's own link-local address, from the development host. */
	zassert_ok(api_parse_ipv6("fe80::8234:28ff:fe10:1273", out));
	zassert_mem_equal(out, link_local, 16);

	zassert_ok(api_parse_ipv6("::", out));
	for (int i = 0; i < 16; i++) {
		zassert_equal(out[i], 0);
	}

	zassert_ok(api_parse_ipv6("2001:db8:0:0:0:0:0:1", out));
	zassert_equal(out[0], 0x20);
	zassert_equal(out[1], 0x01);
	zassert_equal(out[15], 1);

	zassert_ok(api_parse_ipv6("2001:db8::", out));
	zassert_equal(out[15], 0);

	/* An embedded IPv4 tail. */
	zassert_ok(api_parse_ipv6("::ffff:192.0.2.1", out));
	zassert_equal(out[10], 0xff);
	zassert_equal(out[11], 0xff);
	zassert_equal(out[12], 192);
	zassert_equal(out[15], 1);
}

ZTEST(api_validation, test_ipv6_parsing_is_strict)
{
	uint8_t out[16];

	zassert_equal(api_parse_ipv6("2001:db8::1::2", out), -EINVAL, "two compressions");
	zassert_equal(api_parse_ipv6("2001:db8", out), -EINVAL, "too few groups");
	zassert_equal(api_parse_ipv6("2001:db8:0:0:0:0:0:1:2", out), -EINVAL, "too many groups");
	zassert_equal(api_parse_ipv6("2001:db8::1%eth0", out), -EINVAL,
		      "a zone id is not something the resolver can use");
	zassert_equal(api_parse_ipv6("2001:db8::1 ", out), -EINVAL);
	zassert_equal(api_parse_ipv6("2001:dg8::1", out), -EINVAL, "not hex");
	zassert_equal(api_parse_ipv6("2001:db80000::1", out), -EINVAL, "group too long");
	zassert_equal(api_parse_ipv6(":2001:db8::1", out), -EINVAL, "a single leading colon");
	zassert_equal(api_parse_ipv6("2001:db8::1:", out), -EINVAL, "a single trailing colon");
	zassert_equal(api_parse_ipv6("", out), -EINVAL);
	zassert_equal(api_parse_ipv6(NULL, out), -EINVAL);

	/* A compression that stands for no groups at all is not valid, even
	 * though the bytes would come out right.
	 */
	zassert_equal(api_parse_ipv6("1:2:3:4:5:6:7::8", out), -EINVAL);
}

ZTEST(api_validation, test_prefix_bounds_follow_the_schema)
{
	zassert_false(api_ipv4_prefix_is_valid(0));
	zassert_true(api_ipv4_prefix_is_valid(1));
	zassert_true(api_ipv4_prefix_is_valid(24));
	zassert_true(api_ipv4_prefix_is_valid(30));
	zassert_false(api_ipv4_prefix_is_valid(31), "the schema stops at 30");
	zassert_false(api_ipv4_prefix_is_valid(32));
	zassert_false(api_ipv4_prefix_is_valid(255));
}

ZTEST(api_validation, test_same_subnet)
{
	const uint8_t host[4] = {192, 168, 88, 14};
	const uint8_t gateway[4] = {192, 168, 88, 1};
	const uint8_t elsewhere[4] = {192, 168, 89, 1};

	zassert_true(api_ipv4_same_subnet(host, gateway, 24));
	zassert_false(api_ipv4_same_subnet(host, elsewhere, 24));
	zassert_true(api_ipv4_same_subnet(host, elsewhere, 16));
	zassert_true(api_ipv4_same_subnet(host, elsewhere, 22));
	zassert_false(api_ipv4_same_subnet(host, gateway, 30));

	zassert_false(api_ipv4_same_subnet(NULL, gateway, 24));
	zassert_false(api_ipv4_same_subnet(host, gateway, 33));
}

ZTEST(api_validation, test_usable_host_addresses)
{
	const uint8_t ok[4] = {192, 168, 88, 14};
	const uint8_t network[4] = {192, 168, 88, 0};
	const uint8_t broadcast[4] = {192, 168, 88, 255};
	const uint8_t loopback[4] = {127, 0, 0, 1};
	const uint8_t multicast[4] = {239, 255, 0, 1};
	const uint8_t link_local[4] = {169, 254, 1, 1};
	const uint8_t zero[4] = {0, 0, 0, 0};

	zassert_true(api_ipv4_is_usable_host(ok, 24));

	zassert_false(api_ipv4_is_usable_host(network, 24), "the subnet address is no host");
	zassert_false(api_ipv4_is_usable_host(broadcast, 24));
	zassert_false(api_ipv4_is_usable_host(loopback, 24));
	zassert_false(api_ipv4_is_usable_host(zero, 24));
	zassert_false(api_ipv4_is_usable_host(link_local, 24),
		      "169.254/16 is what a failed DHCP assigns by itself");
	zassert_false(api_ipv4_is_usable_host(multicast, 24),
		      "the flood that starved this device came from 239.255/16");

	/* The same address is fine or not depending on the prefix. */
	zassert_true(api_ipv4_is_usable_host(broadcast, 16));
	zassert_false(api_ipv4_is_usable_host(ok, 31), "the schema does not allow /31");
	zassert_false(api_ipv4_is_usable_host(NULL, 24));
}

/* -- the escaper web-api shares with the error body ---------------------- */

ZTEST(api_validation, test_json_append_escaped)
{
	char buf[32];
	int pos;

	memset(buf, 0x55, sizeof(buf));
	pos = api_json_append_escaped(buf, sizeof(buf), 0, "a\"b\\c\n\x01д");
	zassert_equal(pos, (int)strlen("a\\\"b\\\\c\\n\\u0001д"));
	zassert_str_equal(buf, "a\\\"b\\\\c\\n\\u0001д", "and NUL-terminated at the end");

	/* Appending continues from pos. */
	pos = api_json_append_escaped(buf, sizeof(buf), (size_t)pos, "!");
	zassert_str_equal(buf, "a\\\"b\\\\c\\n\\u0001д!");

	/* Exactly fitting, including the NUL. */
	zassert_equal(api_json_append_escaped(buf, 4, 0, "abc"), 3);
	zassert_equal(api_json_append_escaped(buf, 3, 0, "abc"), -ENOMEM, "no room for the NUL");
	zassert_equal(api_json_append_escaped(buf, 4, 0, "a\n"), 3, "a, backslash, n, NUL: fits");
	zassert_equal(api_json_append_escaped(buf, 3, 0, "a\n"), -ENOMEM, "an escape is two bytes");
	zassert_equal(api_json_append_escaped(buf, 4, 4, ""), -ENOMEM, "pos at the end");
	zassert_equal(api_json_append_escaped(NULL, 4, 0, "a"), -ENOMEM);
	zassert_equal(api_json_append_escaped(buf, 4, 0, NULL), -ENOMEM);
	zassert_equal(api_json_append_escaped(buf, sizeof(buf), 0, ""), 0);
	zassert_equal(buf[0], '\0');
}
