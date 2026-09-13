/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * matter-service core over a fake Matter platform.
 *
 * The fake runs nothing on its own: schedule() only queues, and a test drains
 * the queue when it wants the Matter thread to have run. That is how "accepted
 * but not yet run" is a state a test can stand in, and how every read is
 * checked to answer without touching the platform.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <matter_service/matter_service.h>

#define QR "MT:Y.K9042C00KA0648G00"
#define MANUAL "01234567890"
#define PASSCODE "00012345"

/* -- the fake platform ---------------------------------------------------- */

struct work {
	void (*fn)(void *arg);
	void *arg;
};

static struct {
	struct work queue[8];
	size_t queued;
	int schedule_result;

	int64_t now_ms;

	struct matter_window_reading window;
	int open_result;
	uint32_t opened_with_timeout;
	int open_calls;
	int close_calls;

	int codes_result;
	int codes_calls;
	int window_reads;

	struct matter_fabric table[8];
	size_t table_count;
	int fabric_reads;

	uint32_t min_s;
	uint32_t max_s;
} fake;

static int fake_schedule(void (*fn)(void *arg), void *arg)
{
	if (fake.schedule_result != 0) {
		return fake.schedule_result;
	}
	zassert_true(fake.queued < ARRAY_SIZE(fake.queue));
	fake.queue[fake.queued++] = (struct work){fn, arg};
	return 0;
}

static int64_t fake_now(void)
{
	return fake.now_ms;
}

static int fake_open(uint32_t timeout_seconds)
{
	fake.open_calls++;
	fake.opened_with_timeout = timeout_seconds;
	if (fake.open_result == 0) {
		fake.window = (struct matter_window_reading){.open = true};
	}
	return fake.open_result;
}

static void fake_close(void)
{
	fake.close_calls++;
	fake.window = (struct matter_window_reading){0};
}

static void fake_read_window(struct matter_window_reading *out)
{
	fake.window_reads++;
	*out = fake.window;
}

static int fake_read_codes(struct matter_codes *out)
{
	fake.codes_calls++;
	if (fake.codes_result != 0) {
		/* A failing generator may leave half-written fields behind. */
		strcpy(out->qr_payload, "MT:partial");
		return fake.codes_result;
	}
	strcpy(out->qr_payload, QR);
	strcpy(out->manual_pairing_code, MANUAL);
	strcpy(out->setup_passcode, PASSCODE);
	return 0;
}

static size_t fake_read_fabrics(struct matter_fabric *out, size_t max)
{
	size_t n = MIN(max, fake.table_count);

	fake.fabric_reads++;
	memcpy(out, fake.table, n * sizeof(*out));
	return fake.table_count;
}

static uint32_t fake_min(void)
{
	return fake.min_s;
}

static uint32_t fake_max(void)
{
	return fake.max_s;
}

static const struct matter_service_platform platform = {
	.schedule = fake_schedule,
	.now_ms = fake_now,
	.open_basic_window = fake_open,
	.close_window = fake_close,
	.read_window = fake_read_window,
	.read_codes = fake_read_codes,
	.read_fabrics = fake_read_fabrics,
	.min_window_seconds = fake_min,
	.max_window_seconds = fake_max,
};

/** Run everything the service scheduled, in order, as the Matter thread would. */
static void drain(void)
{
	while (fake.queued > 0) {
		struct work w = fake.queue[0];

		memmove(fake.queue, fake.queue + 1, (fake.queued - 1) * sizeof(fake.queue[0]));
		fake.queued--;
		w.fn(w.arg);
	}
}

static struct {
	int calls;
	int result;
	void *ctx;
} done;

static void on_done(void *ctx, int result)
{
	done.calls++;
	done.result = result;
	done.ctx = ctx;
}

static void add_fabric(uint8_t index, uint64_t fabric_id, const char *label)
{
	struct matter_fabric *f = &fake.table[fake.table_count++];

	memset(f, 0, sizeof(*f));
	snprintf(f->id, sizeof(f->id), "00112233445566%02x:%016llx", index,
		 (unsigned long long)fabric_id);
	f->fabric_index = index;
	f->fabric_id = fabric_id;
	f->node_id = 0x1B669ULL + index;
	f->vendor_id = 0xFFF1;
	strcpy(f->label, label);
}

static void ready(void)
{
	matter_service_report_starting();
	matter_service_report_started(0);
}

static void open_web_window(uint32_t timeout)
{
	zassert_equal(matter_service_open_window(timeout, on_done, NULL), MATTER_REQUEST_ACCEPTED);
	drain();
	zassert_equal(done.result, 0);
}

static void *suite_setup(void)
{
	return NULL;
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	memset(&fake, 0, sizeof(fake));
	memset(&done, 0, sizeof(done));
	fake.min_s = 180;
	fake.max_s = 900;
	zassert_ok(matter_service_init(&platform));
}

ZTEST_SUITE(matter_service, NULL, suite_setup, before, NULL, NULL);

/* -- init and lifecycle ----------------------------------------------------- */

ZTEST(matter_service, test_a_platform_with_a_missing_function_is_refused)
{
	struct matter_service_platform partial = platform;

	zassert_equal(matter_service_init(NULL), -EINVAL);
	partial.read_codes = NULL;
	zassert_equal(matter_service_init(&partial), -EINVAL);
	partial = platform;
	partial.schedule = NULL;
	zassert_equal(matter_service_init(&partial), -EINVAL);
}

ZTEST(matter_service, test_before_the_stack_starts_everything_is_unavailable)
{
	struct matter_status status;
	struct matter_window window;
	struct matter_codes codes;
	struct matter_fabric list[3];
	uint32_t min_s, max_s;

	matter_service_get_status(&status);
	zassert_equal(status.state, MATTER_STATE_NOT_READY);
	zassert_false(status.commissioned);
	zassert_equal(status.fabric_count, 0);
	matter_service_get_window(&window);
	zassert_false(window.open);
	zassert_equal(window.source, MATTER_WINDOW_SOURCE_NONE);
	matter_service_get_codes(&codes);
	zassert_equal(codes.reason, MATTER_CODES_SERVICE_NOT_READY);
	zassert_equal(codes.qr_payload[0], '\0');
	zassert_equal(matter_service_get_fabrics(list, ARRAY_SIZE(list)), 0);
	matter_service_window_limits(&min_s, &max_s);
	zassert_equal(min_s, 180);
	zassert_equal(max_s, 900);
}

ZTEST(matter_service, test_starting_then_started_moves_to_ready_and_reads_the_stack)
{
	struct matter_status status;

	matter_service_report_starting();
	matter_service_get_status(&status);
	zassert_equal(status.state, MATTER_STATE_STARTING);

	matter_service_report_started(0);
	matter_service_get_status(&status);
	zassert_equal(status.state, MATTER_STATE_READY);
	zassert_equal(status.failure, 0);
	zassert_equal(fake.window_reads, 1, "the window is read once the stack runs, got %d",
		      fake.window_reads);
	zassert_equal(fake.fabric_reads, 1, "the fabric table is read once the stack runs, got %d",
		      fake.fabric_reads);
	zassert_equal(fake.codes_calls, 1, "the codes are generated before anyone commissions");

	/* A late "starting" report must not take a running stack back. */
	matter_service_report_starting();
	matter_service_get_status(&status);
	zassert_equal(status.state, MATTER_STATE_READY);
}

ZTEST(matter_service, test_a_failed_start_is_reported_with_its_cause)
{
	struct matter_status status;
	struct matter_codes codes;

	matter_service_report_starting();
	matter_service_report_started(-5);
	matter_service_get_status(&status);
	zassert_equal(status.state, MATTER_STATE_FAILED);
	zassert_equal(status.failure, -5);
	zassert_equal(fake.window_reads, 0, "a failed stack is not touched");
	zassert_equal(fake.codes_calls, 0);
	matter_service_get_codes(&codes);
	zassert_equal(codes.reason, MATTER_CODES_SERVICE_NOT_READY);
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_NOT_READY);
}

ZTEST(matter_service, test_requests_before_ready_are_refused_without_scheduling)
{
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_NOT_READY);
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_NOT_READY);
	matter_service_report_starting();
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_NOT_READY);
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_NOT_READY);
	zassert_equal(fake.queued, 0);
}

/* -- opening from the web -------------------------------------------------- */

ZTEST(matter_service, test_an_accepted_open_is_not_open_until_the_matter_thread_runs)
{
	struct matter_window window;
	struct matter_codes codes;
	int ctx_marker;

	ready();
	zassert_equal(matter_service_open_window(300, on_done, &ctx_marker), MATTER_REQUEST_ACCEPTED);
	zassert_equal(fake.open_calls, 0, "nothing happens on the caller's thread");
	matter_service_get_window(&window);
	zassert_false(window.open);
	zassert_equal(done.calls, 0);

	drain();
	zassert_equal(fake.open_calls, 1);
	zassert_equal(fake.opened_with_timeout, 300);
	zassert_equal(done.calls, 1, "the callback runs exactly once");
	zassert_equal(done.result, 0);
	zassert_equal_ptr(done.ctx, &ctx_marker);

	matter_service_get_window(&window);
	zassert_true(window.open);
	zassert_equal(window.mode, MATTER_WINDOW_MODE_BASIC);
	zassert_equal(window.source, MATTER_WINDOW_SOURCE_WEB);
	zassert_equal(window.remaining_seconds, 300);
	zassert_true(window.codes_available);

	matter_service_get_codes(&codes);
	zassert_equal(codes.reason, MATTER_CODES_AVAILABLE);
	zassert_str_equal(codes.qr_payload, QR);
	zassert_str_equal(codes.manual_pairing_code, MANUAL, "leading zeros are kept");
	zassert_str_equal(codes.setup_passcode, PASSCODE, "leading zeros are kept");
}

ZTEST(matter_service, test_a_second_open_while_the_first_waits_is_refused)
{
	ready();
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_ACCEPTED);
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_WINDOW_OPEN);
	drain();
	zassert_equal(fake.open_calls, 1);
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_WINDOW_OPEN,
		      "and once it has run, the window is open");
}

ZTEST(matter_service, test_the_remaining_time_counts_down_and_stops_at_zero)
{
	struct matter_window window;

	ready();
	fake.now_ms = 10000;
	open_web_window(180);

	fake.now_ms += 60500;
	matter_service_get_window(&window);
	zassert_equal(window.remaining_seconds, 120, "whole seconds elapsed, rounded down");

	fake.now_ms += 200000;
	matter_service_get_window(&window);
	zassert_true(window.open, "the window closes when the stack says so, not by arithmetic");
	zassert_equal(window.remaining_seconds, 0);
}

ZTEST(matter_service, test_a_timeout_outside_the_limits_is_refused)
{
	ready();
	zassert_equal(matter_service_open_window(179, on_done, NULL), MATTER_REQUEST_OUT_OF_RANGE);
	zassert_equal(matter_service_open_window(901, on_done, NULL), MATTER_REQUEST_OUT_OF_RANGE);
	zassert_equal(matter_service_open_window(180, on_done, NULL), MATTER_REQUEST_ACCEPTED);
	drain();
	fake_close();
	matter_service_report_window_changed();
	zassert_equal(matter_service_open_window(900, on_done, NULL), MATTER_REQUEST_ACCEPTED);
	zassert_equal(fake.queued, 1);
}

ZTEST(matter_service, test_narrower_sdk_limits_narrow_the_published_range)
{
	uint32_t min_s, max_s;

	fake.min_s = 300;
	fake.max_s = 600;
	ready();
	matter_service_window_limits(&min_s, &max_s);
	zassert_equal(min_s, 300);
	zassert_equal(max_s, 600);
	zassert_equal(matter_service_open_window(299, on_done, NULL), MATTER_REQUEST_OUT_OF_RANGE);
	zassert_equal(matter_service_open_window(601, on_done, NULL), MATTER_REQUEST_OUT_OF_RANGE);
}

ZTEST(matter_service, test_wider_sdk_limits_do_not_widen_the_contract)
{
	uint32_t min_s, max_s;

	/* An uncommissioned device with extended advertising allows 48 hours. */
	fake.min_s = 60;
	fake.max_s = 48 * 3600;
	ready();
	matter_service_window_limits(&min_s, &max_s);
	zassert_equal(min_s, 180);
	zassert_equal(max_s, 900);
}

ZTEST(matter_service, test_a_refusal_from_the_stack_reaches_the_callback_and_leaves_no_window)
{
	struct matter_window window;

	ready();
	fake.open_result = -EBUSY;
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_ACCEPTED);
	drain();
	zassert_equal(done.calls, 1);
	zassert_equal(done.result, -EBUSY);
	matter_service_get_window(&window);
	zassert_false(window.open);

	fake.open_result = 0;
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_ACCEPTED,
		      "a failed open does not leave the service thinking one is pending");
}

ZTEST(matter_service, test_a_window_opened_elsewhere_before_ours_ran_is_not_replaced)
{
	struct matter_window window;

	ready();
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_ACCEPTED);
	fake.window = (struct matter_window_reading){
		.open = true,
		.opened_by_controller = true,
		.controller_mode = MATTER_WINDOW_MODE_ENHANCED,
	};
	drain();
	zassert_equal(fake.open_calls, 0, "the controller's window is left alone");
	zassert_equal(done.result, -EBUSY);
	matter_service_get_window(&window);
	zassert_equal(window.source, MATTER_WINDOW_SOURCE_CONTROLLER);
	zassert_equal(window.mode, MATTER_WINDOW_MODE_ENHANCED);
}

ZTEST(matter_service, test_a_schedule_failure_frees_the_request)
{
	ready();
	fake.schedule_result = -ENOMEM;
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_SCHEDULE_FAILED);
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_SCHEDULE_FAILED);
	fake.schedule_result = 0;
	/* Two slots in this configuration: both must have come back. */
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_ACCEPTED);
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_ACCEPTED);
	zassert_equal(done.calls, 0, "a request that was never queued never calls back");
}

ZTEST(matter_service, test_requests_beyond_the_slots_are_busy)
{
	ready();
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_ACCEPTED);
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_ACCEPTED);
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_BUSY);
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_BUSY);
	drain();
	zassert_equal(done.calls, 2);
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_ACCEPTED);
}

/* -- closing ------------------------------------------------------------------ */

ZTEST(matter_service, test_closing_a_closed_window_succeeds)
{
	ready();
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_ACCEPTED);
	drain();
	zassert_equal(done.calls, 1);
	zassert_equal(done.result, 0);
}

ZTEST(matter_service, test_closing_our_window_clears_it_and_its_codes)
{
	struct matter_window window;
	struct matter_codes codes;

	ready();
	open_web_window(300);
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_ACCEPTED);
	drain();
	zassert_equal(fake.close_calls, 1);
	matter_service_get_window(&window);
	zassert_false(window.open);
	zassert_equal(window.remaining_seconds, 0);
	zassert_false(window.codes_available);
	matter_service_get_codes(&codes);
	zassert_equal(codes.reason, MATTER_CODES_WINDOW_CLOSED);
	zassert_equal(codes.qr_payload[0], '\0');
	zassert_equal(codes.manual_pairing_code[0], '\0');
	zassert_equal(codes.setup_passcode[0], '\0');
}

ZTEST(matter_service, test_a_controller_window_may_be_closed_from_the_web)
{
	struct matter_window window;

	ready();
	fake.window = (struct matter_window_reading){
		.open = true, .opened_by_controller = true, .controller_mode = MATTER_WINDOW_MODE_ENHANCED};
	matter_service_report_window_changed();
	zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_ACCEPTED);
	drain();
	matter_service_get_window(&window);
	zassert_false(window.open);
}

ZTEST(matter_service, test_after_our_window_times_out_a_shell_window_is_not_ours)
{
	struct matter_window window;

	ready();
	open_web_window(300);
	/* The SDK's timer closes it. */
	fake.window = (struct matter_window_reading){0};
	matter_service_report_window_changed();
	matter_service_get_window(&window);
	zassert_false(window.open);

	/* Later someone opens one from the debug shell. */
	fake.window = (struct matter_window_reading){.open = true};
	matter_service_report_window_changed();
	matter_service_get_window(&window);
	zassert_true(window.open);
	zassert_equal(window.source, MATTER_WINDOW_SOURCE_LOCAL);
	zassert_equal(window.remaining_seconds, 0, "its timeout is not known");
	zassert_true(window.codes_available, "a basic window uses the device's own codes");
}

/* -- windows opened by others ------------------------------------------------ */

ZTEST(matter_service, test_an_enhanced_controller_window_has_no_codes)
{
	struct matter_window window;
	struct matter_codes codes;

	ready();
	fake.window = (struct matter_window_reading){
		.open = true, .opened_by_controller = true, .controller_mode = MATTER_WINDOW_MODE_ENHANCED};
	matter_service_report_window_changed();

	matter_service_get_window(&window);
	zassert_true(window.open);
	zassert_equal(window.source, MATTER_WINDOW_SOURCE_CONTROLLER);
	zassert_equal(window.mode, MATTER_WINDOW_MODE_ENHANCED);
	zassert_false(window.codes_available);
	zassert_equal(window.remaining_seconds, 0);

	matter_service_get_codes(&codes);
	zassert_equal(codes.reason, MATTER_CODES_PASSCODE_UNAVAILABLE);
	zassert_equal(codes.qr_payload[0], '\0', "no factory codes stand in for the real ones");
	zassert_equal(matter_service_open_window(300, on_done, NULL), MATTER_REQUEST_WINDOW_OPEN);
}

ZTEST(matter_service, test_a_basic_controller_window_shows_the_devices_codes)
{
	struct matter_window window;

	ready();
	fake.window = (struct matter_window_reading){
		.open = true, .opened_by_controller = true, .controller_mode = MATTER_WINDOW_MODE_BASIC};
	matter_service_report_window_changed();
	matter_service_get_window(&window);
	zassert_equal(window.source, MATTER_WINDOW_SOURCE_CONTROLLER);
	zassert_equal(window.mode, MATTER_WINDOW_MODE_BASIC);
	zassert_true(window.codes_available);
}

ZTEST(matter_service, test_codes_that_fail_to_generate_are_not_shown_half_written)
{
	struct matter_window window;
	struct matter_codes codes;

	/* The codes are generated when the stack starts, so that is where it fails. */
	fake.codes_result = -EIO;
	ready();
	open_web_window(300);
	matter_service_get_window(&window);
	zassert_true(window.open);
	zassert_false(window.codes_available);
	matter_service_get_codes(&codes);
	zassert_equal(codes.reason, MATTER_CODES_GENERATION_FAILED);
	zassert_equal(codes.qr_payload[0], '\0');
}

/* -- fabrics ------------------------------------------------------------------- */

ZTEST(matter_service, test_fabrics_are_listed_in_index_order_and_make_the_device_commissioned)
{
	struct matter_fabric list[3];
	struct matter_status status;

	add_fabric(3, 0xFAB0000000000003ULL, "lab");
	add_fabric(1, 0xFAB0000000000001ULL, "");
	ready();

	matter_service_get_status(&status);
	zassert_true(status.commissioned);
	zassert_equal(status.fabric_count, 2);
	zassert_equal(matter_service_get_fabrics(list, ARRAY_SIZE(list)), 2);
	zassert_equal(list[0].fabric_index, 1);
	zassert_str_equal(list[0].label, "", "an empty label is a real answer");
	zassert_equal(list[1].fabric_index, 3);
	zassert_equal(list[1].fabric_id, 0xFAB0000000000003ULL);
}

ZTEST(matter_service, test_a_committed_fabric_appears_without_losing_the_first)
{
	struct matter_fabric list[3];

	add_fabric(1, 0xAAULL, "home");
	ready();
	add_fabric(2, 0xBBULL, "");
	matter_service_report_fabrics_changed();
	zassert_equal(matter_service_get_fabrics(list, ARRAY_SIZE(list)), 2);
	zassert_str_equal(list[0].label, "home");
	zassert_equal(list[1].fabric_id, 0xBBULL);
}

ZTEST(matter_service, test_a_removed_fabric_disappears)
{
	struct matter_status status;

	add_fabric(1, 0xAAULL, "home");
	ready();
	fake.table_count = 0;
	matter_service_report_fabrics_changed();
	matter_service_get_status(&status);
	zassert_false(status.commissioned);
	zassert_equal(status.fabric_count, 0);
}

ZTEST(matter_service, test_the_snapshot_holds_at_most_its_configured_fabrics)
{
	struct matter_fabric list[8];

	for (uint8_t i = 1; i <= 5; i++) {
		add_fabric(i, i, "");
	}
	ready();
	zassert_equal(matter_service_get_fabrics(list, ARRAY_SIZE(list)), 3);
	zassert_equal(matter_service_get_fabrics(list, 2), 2, "the caller's bound is respected");
}

ZTEST(matter_service, test_strings_from_the_adapter_are_terminated)
{
	struct matter_fabric list[1];

	add_fabric(1, 1, "");
	memset(fake.table[0].label, 'x', sizeof(fake.table[0].label));
	memset(fake.table[0].id, 'y', sizeof(fake.table[0].id));
	ready();
	zassert_equal(matter_service_get_fabrics(list, 1), 1);
	zassert_equal(strlen(list[0].label), MATTER_FABRIC_LABEL_MAX_LEN);
	zassert_equal(strlen(list[0].id), MATTER_FABRIC_ID_MAX_LEN);
}

/* -- the caller never waits --------------------------------------------------- */

ZTEST(matter_service, test_reads_never_touch_the_matter_stack)
{
	struct matter_status status;
	struct matter_window window;
	struct matter_codes codes;
	struct matter_fabric list[3];
	uint32_t min_s, max_s;

	add_fabric(1, 1, "");
	ready();
	open_web_window(300);

	int window_reads = fake.window_reads;
	int fabric_reads = fake.fabric_reads;
	int codes_calls = fake.codes_calls;

	for (int i = 0; i < 3; i++) {
		matter_service_get_status(&status);
		matter_service_get_window(&window);
		matter_service_get_codes(&codes);
		(void)matter_service_get_fabrics(list, ARRAY_SIZE(list));
		matter_service_window_limits(&min_s, &max_s);
	}
	zassert_equal(fake.window_reads, window_reads);
	zassert_equal(fake.fabric_reads, fabric_reads);
	zassert_equal(fake.codes_calls, codes_calls);
	zassert_equal(fake.queued, 0, "and schedule nothing");
}

/* -- names ------------------------------------------------------------------------ */

ZTEST(matter_service, test_names_are_the_documents_enum_values)
{
	zassert_str_equal(matter_state_str(MATTER_STATE_NOT_READY), "not_ready");
	zassert_str_equal(matter_state_str(MATTER_STATE_STARTING), "starting");
	zassert_str_equal(matter_state_str(MATTER_STATE_READY), "ready");
	zassert_str_equal(matter_state_str(MATTER_STATE_FAILED), "failed");
	zassert_str_equal(matter_window_mode_str(MATTER_WINDOW_MODE_BASIC), "basic");
	zassert_str_equal(matter_window_mode_str(MATTER_WINDOW_MODE_ENHANCED), "enhanced");
	zassert_is_null(matter_window_mode_str(MATTER_WINDOW_MODE_NONE));
	zassert_str_equal(matter_window_source_str(MATTER_WINDOW_SOURCE_WEB), "web");
	zassert_str_equal(matter_window_source_str(MATTER_WINDOW_SOURCE_CONTROLLER), "controller");
	zassert_str_equal(matter_window_source_str(MATTER_WINDOW_SOURCE_LOCAL), "local");
	zassert_is_null(matter_window_source_str(MATTER_WINDOW_SOURCE_NONE));
	zassert_str_equal(matter_codes_reason_str(MATTER_CODES_WINDOW_CLOSED), "window_closed");
	zassert_str_equal(matter_codes_reason_str(MATTER_CODES_PASSCODE_UNAVAILABLE),
			  "passcode_unavailable");
	zassert_str_equal(matter_codes_reason_str(MATTER_CODES_SERVICE_NOT_READY), "service_not_ready");
	zassert_str_equal(matter_codes_reason_str(MATTER_CODES_GENERATION_FAILED), "generation_failed");
	zassert_is_null(matter_codes_reason_str(MATTER_CODES_AVAILABLE));
}

/* -- codes are generated once ----------------------------------------------- */

ZTEST(matter_service, test_codes_are_generated_once_and_reused_by_every_window)
{
	struct matter_codes codes;

	ready();
	zassert_equal(fake.codes_calls, 1);

	for (int i = 0; i < 3; i++) {
		open_web_window(300);
		matter_service_report_window_changed();
		zassert_equal(matter_service_close_window(on_done, NULL), MATTER_REQUEST_ACCEPTED);
		drain();
	}
	/* A controller's basic window, and the events around a commissioning. */
	fake.window = (struct matter_window_reading){
		.open = true, .opened_by_controller = true, .controller_mode = MATTER_WINDOW_MODE_BASIC};
	matter_service_report_window_changed();
	matter_service_report_window_changed();
	matter_service_get_codes(&codes);
	zassert_equal(codes.reason, MATTER_CODES_AVAILABLE);
	zassert_str_equal(codes.manual_pairing_code, MANUAL);
	zassert_equal(fake.codes_calls, 1, "never regenerated, got %d", fake.codes_calls);
}

ZTEST(matter_service, test_a_failed_generation_is_retried_by_the_next_window)
{
	struct matter_codes codes;

	fake.codes_result = -EIO;
	ready();
	zassert_equal(fake.codes_calls, 1);

	fake.codes_result = 0;
	open_web_window(300);
	matter_service_get_codes(&codes);
	zassert_equal(codes.reason, MATTER_CODES_AVAILABLE);
	zassert_str_equal(codes.setup_passcode, PASSCODE);
	int calls = fake.codes_calls;

	matter_service_report_window_changed();
	zassert_equal(fake.codes_calls, calls, "and once it worked, it is kept");
}
