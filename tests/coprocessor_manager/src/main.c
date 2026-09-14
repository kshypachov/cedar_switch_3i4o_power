/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * coprocessor-manager unit tests.
 *
 * Section 12 asks this tier for the UART ownership automaton, a failed stop of
 * the receive interrupt, and the mutual exclusion of modes and operations.
 * The platform is tests/fakes/fake_uart.c, which can keep an interrupt firing
 * after its handler is gone, refuse an attach or a reset, and run a claim in
 * the middle of a switch. Nothing here waits except the one test that needs a
 * second thread.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <coprocessor_manager/coprocessor_manager.h>

#include "fake_uart.h"

#define CONSOLE     COPROCESSOR_UART_CONSOLE
#define BRIDGE      COPROCESSOR_UART_USB_BRIDGE
#define FLASHING    COPROCESSOR_UART_FLASHING
#define UNAVAILABLE COPROCESSOR_UART_UNAVAILABLE
#define APPLY       COPROCESSOR_CLAIM_NETWORK_APPLY
#define SCAN        COPROCESSOR_CLAIM_WIFI_SCAN

#define STEP_MS         CONFIG_COPROCESSOR_MANAGER_RX_QUIET_STEP_MS
#define STOP_TIMEOUT_MS CONFIG_COPROCESSOR_MANAGER_RX_STOP_TIMEOUT_MS
#define WINDOW_MS       CONFIG_COPROCESSOR_MANAGER_RESET_WINDOW_MS

static void case_before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_uart_init();
	fake_uart.now_ms = 10000;
	zassert_ok(coprocessor_manager_init(&fake_uart_platform));
}

ZTEST_SUITE(coprocessor_manager, NULL, NULL, case_before, NULL, NULL);

/* --- helpers ------------------------------------------------------------ */

static struct coprocessor_status status(void)
{
	struct coprocessor_status s;

	coprocessor_manager_get_status(&s);
	return s;
}

static void assert_marker(size_t i, enum log_store_kind kind, uint32_t generation,
			  const char *text)
{
	zassert_true(i < fake_uart.marker_count, "marker %zu was not written", i);
	zassert_equal(fake_uart.markers[i].kind, kind, "marker %zu kind", i);
	zassert_equal(fake_uart.markers[i].generation, generation, "marker %zu generation", i);
	zassert_str_equal(fake_uart.markers[i].text, text, "marker %zu text", i);
}

/* Start over in @p mode, with no markers recorded. */
static void start_in(enum coprocessor_uart_mode mode)
{
	fake_uart_init();
	fake_uart.now_ms = 10000;
	if (mode == UNAVAILABLE) {
		fake_uart.attach_errno[CONSOLE] = -EIO;
		zassert_equal(coprocessor_manager_init(&fake_uart_platform), -EIO);
		fake_uart.attach_errno[CONSOLE] = 0;
	} else {
		zassert_ok(coprocessor_manager_init(&fake_uart_platform));
		zassert_ok(coprocessor_manager_set_mode(mode));
	}
	fake_uart.marker_count = 0;
}

/* --- init --------------------------------------------------------------- */

ZTEST(coprocessor_manager, test_init_gives_the_uart_to_the_console_at_generation_one)
{
	struct coprocessor_status s = status();

	zassert_equal(s.uart_mode, CONSOLE);
	zassert_equal(s.generation, 1, "the driver's init reset the C6");
	zassert_equal(s.last_switch_error, 0);
	zassert_equal(s.switches, 0);
	zassert_true(fake_uart.handler);
	zassert_equal(fake_uart.owner, CONSOLE);
	zassert_equal(fake_uart.marker_count, 0);

	zassert_false(s.transport_ready);
	fake_uart.transport_ready = true;
	zassert_true(status().transport_ready, "read from the platform, not from a copy");
}

ZTEST(coprocessor_manager, test_a_console_that_cannot_attach_at_init_is_unavailable)
{
	fake_uart_init();
	fake_uart.attach_errno[CONSOLE] = -EIO;
	zassert_equal(coprocessor_manager_init(&fake_uart_platform), -EIO);

	struct coprocessor_status s = status();

	zassert_equal(s.uart_mode, UNAVAILABLE);
	zassert_equal(s.last_switch_error, -EIO);
	zassert_false(fake_uart.handler);
	zassert_str_equal(coprocessor_logs_unavailable_reason(s.uart_mode), "uart_unavailable");
}

ZTEST(coprocessor_manager, test_init_refuses_an_incomplete_platform)
{
	static struct coprocessor_platform p;
	void **fields[] = {
		(void **)&p.uart_attach, (void **)&p.uart_detach, (void **)&p.uart_rx_activity,
		(void **)&p.c6_reset,    (void **)&p.marker,      (void **)&p.now_ms,
		(void **)&p.sleep_ms,
	};

	zassert_equal(coprocessor_manager_init(NULL), -EINVAL);
	for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
		p = fake_uart_platform;
		*fields[i] = NULL;
		zassert_equal(coprocessor_manager_init(&p), -EINVAL, "field %zu", i);
	}
	zassert_equal(status().uart_mode, CONSOLE, "a refused init changed nothing");

	p = fake_uart_platform;
	p.transport_ready = NULL;
	fake_uart.transport_ready = true;
	zassert_ok(coprocessor_manager_init(&p), "the transport is optional");
	zassert_false(status().transport_ready, "and without it never ready");
}

/* --- the automaton ------------------------------------------------------ */

ZTEST(coprocessor_manager, test_unavailable_and_unknown_modes_are_not_requested)
{
	zassert_equal(coprocessor_manager_set_mode(UNAVAILABLE), -EINVAL);
	zassert_equal(coprocessor_manager_set_mode((enum coprocessor_uart_mode)42), -EINVAL);
	zassert_equal(fake_uart.detach_calls, 0);
	zassert_equal(status().uart_mode, CONSOLE);
}

ZTEST(coprocessor_manager, test_asking_for_the_current_mode_does_nothing)
{
	zassert_ok(coprocessor_manager_set_mode(CONSOLE));
	zassert_equal(fake_uart.detach_calls, 0);
	zassert_equal(fake_uart.marker_count, 0);
	zassert_equal(status().generation, 1);
	zassert_equal(status().switches, 0);
}

ZTEST(coprocessor_manager, test_console_to_usb_bridge_and_back)
{
	zassert_ok(coprocessor_manager_set_mode(BRIDGE));

	struct coprocessor_status s = status();

	zassert_equal(s.uart_mode, BRIDGE);
	zassert_equal(s.generation, 1);
	zassert_equal(s.switches, 1);
	zassert_equal(fake_uart.owner, BRIDGE);
	zassert_true(fake_uart.handler);
	zassert_equal(fake_uart.marker_count, 1);
	assert_marker(0, LOG_STORE_KIND_PAUSED, 1, "UART handed to the USB bridge");
	zassert_str_equal(coprocessor_logs_unavailable_reason(s.uart_mode), "uart_usb_bridge");

	fake_uart_receive(10);
	zassert_equal(fake_uart.bridge_bytes, 10);
	zassert_equal(fake_uart.console_bytes, 0, "the log does not read what the bridge owns");

	zassert_ok(coprocessor_manager_set_mode(CONSOLE));
	s = status();
	zassert_equal(s.uart_mode, CONSOLE);
	zassert_equal(s.generation, 2, "the UART came back: a new generation");
	zassert_equal(s.switches, 2);
	zassert_equal(fake_uart.marker_count, 2);
	assert_marker(1, LOG_STORE_KIND_RESET, 2, "UART returned to the log");
	zassert_is_null(coprocessor_logs_unavailable_reason(s.uart_mode));
}

/*
 * The P6 path with a stand-in for esp-serial-flasher: the flasher gets a UART
 * nobody else reads, and the log records that it paused and came back.
 */
ZTEST(coprocessor_manager, test_a_flasher_gets_the_uart_to_itself_and_gives_it_back)
{
	zassert_equal(fake_uart_flasher_open(), -EBUSY, "not while the console reads it");
	fake_uart.double_ownership = 0;

	zassert_ok(coprocessor_manager_set_mode(FLASHING));
	zassert_equal(status().uart_mode, FLASHING);
	zassert_false(fake_uart.handler, "no handler of anybody's is left installed");
	zassert_str_equal(coprocessor_logs_unavailable_reason(FLASHING), "uart_flashing");

	zassert_ok(fake_uart_flasher_open());
	fake_uart_receive(64);
	zassert_equal(fake_uart.flasher_bytes, 64);
	zassert_equal(fake_uart.console_bytes, 0);
	fake_uart_flasher_close();

	zassert_ok(coprocessor_manager_set_mode(CONSOLE));
	zassert_equal(fake_uart.double_ownership, 0);
	zassert_equal(fake_uart.marker_count, 2);
	assert_marker(0, LOG_STORE_KIND_PAUSED, 1, "UART handed to the flasher");
	assert_marker(1, LOG_STORE_KIND_RESET, 2, "UART returned to the log");

	fake_uart_receive(5);
	zassert_equal(fake_uart.console_bytes, 5, "the log reads again");
}

ZTEST(coprocessor_manager, test_every_pair_of_modes)
{
	static const enum coprocessor_uart_mode modes[] = {CONSOLE, BRIDGE, FLASHING, UNAVAILABLE};

	ARRAY_FOR_EACH(modes, i) {
		ARRAY_FOR_EACH(modes, j) {
			const enum coprocessor_uart_mode from = modes[i];
			const enum coprocessor_uart_mode to = modes[j];
			bool programmers = (from == BRIDGE && to == FLASHING) ||
					   (from == FLASHING && to == BRIDGE);
			int expected = (to == UNAVAILABLE)  ? -EINVAL
				       : (from == to)      ? 0
				       : programmers       ? -EBUSY
							   : 0;
			const bool moved = expected == 0 && from != to;

			start_in(from);

			const uint32_t generation = status().generation;

			zassert_equal(coprocessor_manager_set_mode(to), expected, "%d -> %d", from, to);

			const enum coprocessor_uart_mode now = moved ? to : from;
			struct coprocessor_status s = status();

			zassert_equal(s.uart_mode, now, "%d -> %d", from, to);
			zassert_equal(fake_uart.handler, now == CONSOLE || now == BRIDGE, "%d -> %d",
				      from, to);
			zassert_equal(fake_uart.marker_count,
				      moved ? (size_t)(from == CONSOLE) + (size_t)(to == CONSOLE) : 0,
				      "%d -> %d", from, to);
			zassert_equal(s.generation, generation + (moved && to == CONSOLE),
				      "%d -> %d", from, to);
		}
	}
}

/*
 * Every byte the console received before the handover must land in front of
 * the pause, and no byte of the new generation in front of the reset.
 */
ZTEST(coprocessor_manager, test_markers_sit_on_the_right_side_of_the_handover)
{
	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	zassert_ok(coprocessor_manager_set_mode(CONSOLE));

	zassert_equal(fake_uart.marker_count, 2);
	zassert_false(fake_uart.markers[0].handler_attached,
		      "the pause is written after the console stopped");
	zassert_false(fake_uart.markers[1].handler_attached,
		      "the reset is written before the console reads again");
}

/* --- stopping the receive interrupt -------------------------------------- */

ZTEST(coprocessor_manager, test_a_switch_whose_interrupt_stops_at_once_takes_two_intervals)
{
	const int64_t start = fake_uart.now_ms;

	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	zassert_equal(fake_uart.now_ms - start, 2 * STEP_MS);
}

ZTEST(coprocessor_manager, test_an_interrupt_that_settles_in_time_is_no_failure)
{
	fake_uart.noisy_reads = 3;
	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	zassert_equal(status().rx_stop_failures, 0);
	zassert_equal(status().uart_mode, BRIDGE);
}

/*
 * Section 8: handing a UART that is still being read to a second reader shows
 * up as corrupted traffic, not as an error. So it is not handed over at all.
 */
ZTEST(coprocessor_manager, test_an_interrupt_that_will_not_stop_keeps_its_owner)
{
	const int64_t start = fake_uart.now_ms;

	fake_uart.noisy_reads = FAKE_UART_NEVER_QUIET;
	zassert_equal(coprocessor_manager_set_mode(BRIDGE), -ETIMEDOUT);

	struct coprocessor_status s = status();

	zassert_equal(s.uart_mode, CONSOLE);
	zassert_equal(fake_uart.owner, CONSOLE);
	zassert_true(fake_uart.handler, "the console got its handler back");
	zassert_equal(s.rx_stop_failures, 1);
	zassert_equal(s.last_switch_error, -ETIMEDOUT);
	zassert_equal(s.switches, 0);
	zassert_equal(s.generation, 1);
	zassert_equal(fake_uart.marker_count, 0, "nothing was handed over, nothing is logged");
	zassert_true(fake_uart.now_ms - start >= STOP_TIMEOUT_MS);
	zassert_true(fake_uart.now_ms - start <= STOP_TIMEOUT_MS + STEP_MS, "and no longer");

	zassert_ok(coprocessor_manager_claim(APPLY), "the failed switch left no intent behind");
}

ZTEST(coprocessor_manager, test_a_bridge_whose_interrupt_will_not_stop_stays_the_bridge)
{
	start_in(BRIDGE);
	fake_uart.noisy_reads = FAKE_UART_NEVER_QUIET;

	zassert_equal(coprocessor_manager_set_mode(CONSOLE), -ETIMEDOUT);
	zassert_equal(status().uart_mode, BRIDGE);
	zassert_equal(fake_uart.owner, BRIDGE);
	zassert_true(fake_uart.handler);
	zassert_equal(status().generation, 1);
	zassert_equal(fake_uart.marker_count, 0);
	zassert_equal(coprocessor_manager_claim(APPLY), -EBUSY, "the bridge still excludes an apply");
}

ZTEST(coprocessor_manager, test_a_detach_that_fails_is_a_failed_stop)
{
	fake_uart.detach_errno = -EIO;
	zassert_equal(coprocessor_manager_set_mode(FLASHING), -ETIMEDOUT);
	zassert_equal(status().uart_mode, CONSOLE);
	zassert_equal(status().rx_stop_failures, 1);
	zassert_true(fake_uart.handler);
}

ZTEST(coprocessor_manager, test_a_console_that_cannot_be_restored_after_a_failed_stop)
{
	fake_uart.noisy_reads = FAKE_UART_NEVER_QUIET;
	fake_uart.attach_errno[CONSOLE] = -EIO;

	zassert_equal(coprocessor_manager_set_mode(BRIDGE), -ETIMEDOUT);
	zassert_equal(status().uart_mode, UNAVAILABLE);
	zassert_false(fake_uart.handler);
	zassert_equal(fake_uart.marker_count, 1);
	assert_marker(0, LOG_STORE_KIND_PAUSED, 1, "UART could not be given back to the log");
}

/* --- attaching the new owner -------------------------------------------- */

ZTEST(coprocessor_manager, test_a_bridge_that_cannot_attach_gives_the_console_back)
{
	fake_uart.attach_errno[BRIDGE] = -EIO;

	zassert_equal(coprocessor_manager_set_mode(BRIDGE), -EIO);

	struct coprocessor_status s = status();

	zassert_equal(s.uart_mode, CONSOLE);
	zassert_equal(fake_uart.owner, CONSOLE);
	zassert_equal(s.last_switch_error, -EIO);
	zassert_equal(s.switches, 0);
	zassert_equal(fake_uart.marker_count, 2);
	assert_marker(0, LOG_STORE_KIND_PAUSED, 1, "UART handed to the USB bridge");
	assert_marker(1, LOG_STORE_KIND_RESET, 2, "UART returned to the log");
	zassert_ok(coprocessor_manager_claim(APPLY), "no bridge is left to exclude it");
}

ZTEST(coprocessor_manager, test_when_nobody_can_take_the_uart_it_is_unavailable)
{
	fake_uart.attach_errno[BRIDGE] = -EIO;
	fake_uart.attach_errno[CONSOLE] = -EIO;

	zassert_equal(coprocessor_manager_set_mode(BRIDGE), -EIO);
	zassert_equal(status().uart_mode, UNAVAILABLE);
	zassert_false(fake_uart.handler);
	zassert_equal(fake_uart.marker_count, 3);
	assert_marker(2, LOG_STORE_KIND_PAUSED, 2, "UART could not be given back to the log");

	fake_uart.attach_errno[BRIDGE] = 0;
	fake_uart.attach_errno[CONSOLE] = 0;
	zassert_ok(coprocessor_manager_set_mode(CONSOLE), "unavailable is left by asking again");
	zassert_equal(status().uart_mode, CONSOLE);
	zassert_equal(status().generation, 3);
	assert_marker(3, LOG_STORE_KIND_RESET, 3, "UART returned to the log");
	zassert_equal(status().last_switch_error, 0);
}

ZTEST(coprocessor_manager, test_a_console_that_cannot_come_back_leaves_the_bridge_in_place)
{
	start_in(BRIDGE);
	fake_uart.attach_errno[CONSOLE] = -EIO;

	zassert_equal(coprocessor_manager_set_mode(CONSOLE), -EIO);
	zassert_equal(status().uart_mode, BRIDGE);
	zassert_equal(fake_uart.owner, BRIDGE);
	zassert_equal(fake_uart.marker_count, 2);
	assert_marker(0, LOG_STORE_KIND_RESET, 2, "UART returned to the log");
	assert_marker(1, LOG_STORE_KIND_PAUSED, 2, "UART could not be given back to the log");
	zassert_equal(coprocessor_manager_claim(APPLY), -EBUSY);
}

/* --- mutual exclusion --------------------------------------------------- */

ZTEST(coprocessor_manager, test_an_apply_excludes_the_bridge_the_flasher_and_a_reset)
{
	zassert_ok(coprocessor_manager_claim(APPLY));

	zassert_equal(coprocessor_manager_set_mode(BRIDGE), -EBUSY);
	zassert_equal(coprocessor_manager_set_mode(FLASHING), -EBUSY);
	zassert_equal(coprocessor_manager_reset(false), -EBUSY);
	zassert_equal(coprocessor_manager_reset(true), -EBUSY);
	zassert_equal(fake_uart.detach_calls, 0, "refused before anything was touched");
	zassert_equal(fake_uart.reset_calls, 0);
	zassert_equal(fake_uart.marker_count, 0);
	zassert_equal(status().uart_mode, CONSOLE);
	zassert_equal(status().last_switch_error, 0, "a refusal is not a failed switch");

	coprocessor_manager_release(APPLY);
	zassert_ok(coprocessor_manager_reset(false));
	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
}

ZTEST(coprocessor_manager, test_the_bridge_excludes_an_apply_but_not_a_scan)
{
	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	zassert_equal(coprocessor_manager_claim(APPLY), -EBUSY);
	zassert_ok(coprocessor_manager_claim(SCAN), "scanning uses SPI, not the UART");
	zassert_ok(coprocessor_manager_reset(false), "a reset in the bridge is allowed");

	zassert_ok(coprocessor_manager_set_mode(CONSOLE), "a scan does not hold the console back");
	zassert_ok(coprocessor_manager_claim(APPLY), "leaving the bridge releases the exclusion");
}

ZTEST(coprocessor_manager, test_the_flasher_excludes_an_apply_and_a_scan)
{
	zassert_ok(coprocessor_manager_set_mode(FLASHING));
	zassert_equal(coprocessor_manager_claim(APPLY), -EBUSY);
	zassert_equal(coprocessor_manager_claim(SCAN), -EBUSY);
	zassert_equal(coprocessor_manager_reset(false), -EBUSY, "the flasher drives EN itself");
	zassert_equal(fake_uart.reset_calls, 0);

	zassert_ok(coprocessor_manager_set_mode(CONSOLE));
	zassert_ok(coprocessor_manager_claim(APPLY));
	zassert_ok(coprocessor_manager_claim(SCAN));
}

ZTEST(coprocessor_manager, test_a_scan_excludes_the_flasher_only)
{
	zassert_ok(coprocessor_manager_claim(SCAN));

	zassert_equal(coprocessor_manager_set_mode(FLASHING), -EBUSY);
	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	zassert_ok(coprocessor_manager_set_mode(CONSOLE));
	zassert_ok(coprocessor_manager_reset(false));

	coprocessor_manager_release(SCAN);
	zassert_ok(coprocessor_manager_set_mode(FLASHING));
}

ZTEST(coprocessor_manager, test_the_programmers_do_not_take_the_chip_from_each_other)
{
	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	zassert_equal(coprocessor_manager_set_mode(FLASHING), -EBUSY);
	zassert_equal(status().uart_mode, BRIDGE);
	zassert_equal(fake_uart.detach_calls, 1, "only the first switch detached anything");

	zassert_ok(coprocessor_manager_set_mode(CONSOLE));
	zassert_ok(coprocessor_manager_set_mode(FLASHING));
	zassert_equal(coprocessor_manager_set_mode(BRIDGE), -EBUSY);
	zassert_equal(status().uart_mode, FLASHING);
}

static int claim_rc;

static void claim_apply(void *arg)
{
	ARG_UNUSED(arg);
	claim_rc = coprocessor_manager_claim(APPLY);
}

/* Claim-then-check from the other side: the switch marked itself first. */
ZTEST(coprocessor_manager, test_a_claim_arriving_during_a_switch_is_refused)
{
	claim_rc = 0;
	fake_uart.during_call = claim_apply;

	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	zassert_equal(claim_rc, -EBUSY);
	zassert_equal(status().uart_mode, BRIDGE);
}

ZTEST(coprocessor_manager, test_a_claim_arriving_during_a_reset_is_refused)
{
	claim_rc = 0;
	fake_uart.during_call = claim_apply;

	zassert_ok(coprocessor_manager_reset(false));
	zassert_equal(claim_rc, -EBUSY);
	zassert_ok(coprocessor_manager_claim(APPLY), "the reset is over");
}

ZTEST(coprocessor_manager, test_a_claim_arriving_during_a_switch_that_fails_is_granted_after)
{
	claim_rc = 0;
	fake_uart.during_call = claim_apply;
	fake_uart.noisy_reads = FAKE_UART_NEVER_QUIET;

	zassert_equal(coprocessor_manager_set_mode(FLASHING), -ETIMEDOUT);
	zassert_equal(claim_rc, -EBUSY);
	zassert_ok(coprocessor_manager_claim(APPLY));
}

ZTEST(coprocessor_manager, test_a_refused_switch_leaves_no_intent_behind)
{
	zassert_ok(coprocessor_manager_claim(APPLY));
	zassert_equal(coprocessor_manager_set_mode(FLASHING), -EBUSY);
	coprocessor_manager_release(APPLY);

	zassert_ok(coprocessor_manager_claim(SCAN), "no flasher is on its way");
	zassert_ok(coprocessor_manager_claim(APPLY));
}

ZTEST(coprocessor_manager, test_claims_are_counted)
{
	zassert_ok(coprocessor_manager_claim(SCAN));
	zassert_ok(coprocessor_manager_claim(SCAN));
	coprocessor_manager_release(SCAN);
	zassert_equal(coprocessor_manager_set_mode(FLASHING), -EBUSY, "one scan is still running");
	coprocessor_manager_release(SCAN);
	zassert_ok(coprocessor_manager_set_mode(FLASHING));
}

ZTEST(coprocessor_manager, test_a_release_without_a_claim_does_not_cancel_the_next_claim)
{
	coprocessor_manager_release(APPLY);
	coprocessor_manager_release(APPLY);
	zassert_ok(coprocessor_manager_claim(APPLY));
	zassert_equal(coprocessor_manager_set_mode(BRIDGE), -EBUSY);
}

ZTEST(coprocessor_manager, test_unknown_claims_are_refused)
{
	zassert_equal(coprocessor_manager_claim(COPROCESSOR_CLAIM_COUNT), -EINVAL);
	coprocessor_manager_release(COPROCESSOR_CLAIM_COUNT);
	zassert_ok(coprocessor_manager_set_mode(FLASHING));
}

ZTEST(coprocessor_manager, test_init_clears_every_claim)
{
	zassert_ok(coprocessor_manager_claim(APPLY));
	zassert_ok(coprocessor_manager_claim(SCAN));
	zassert_ok(coprocessor_manager_init(&fake_uart_platform));
	zassert_ok(coprocessor_manager_set_mode(FLASHING));
}

/* --- resetting the C6 ---------------------------------------------------- */

ZTEST(coprocessor_manager, test_a_reset_starts_a_generation_and_says_how)
{
	zassert_ok(coprocessor_manager_reset(false));
	zassert_equal(fake_uart.reset_calls, 1);
	zassert_false(fake_uart.last_reset_download);
	zassert_equal(status().generation, 2);
	zassert_equal(status().resets, 1);
	assert_marker(0, LOG_STORE_KIND_RESET, 2, "ESP32 reset through EN");

	zassert_ok(coprocessor_manager_reset(true));
	zassert_true(fake_uart.last_reset_download);
	zassert_equal(status().generation, 3);
	zassert_equal(status().resets, 2);
	assert_marker(1, LOG_STORE_KIND_RESET, 3, "ESP32 reset into its ROM loader through EN");
	zassert_equal(status().uart_mode, CONSOLE, "a reset does not move the UART");
}

ZTEST(coprocessor_manager, test_a_pulse_that_fails_starts_no_generation)
{
	fake_uart.reset_errno = -EIO;

	zassert_equal(coprocessor_manager_reset(false), -EIO);
	zassert_equal(status().generation, 1);
	zassert_equal(status().resets, 0);
	zassert_equal(fake_uart.marker_count, 0);
	zassert_ok(coprocessor_manager_claim(APPLY), "the reset's intent was cleared");
}

/* --- ROM banners -------------------------------------------------------- */

ZTEST(coprocessor_manager, test_a_banner_in_the_window_after_init_is_the_drivers_reset)
{
	fake_uart.now_ms += WINDOW_MS - 1;
	coprocessor_manager_note_banner();
	zassert_equal(status().generation, 1);
	zassert_equal(status().unexpected_resets, 0);
	zassert_equal(fake_uart.marker_count, 0);

	fake_uart.now_ms += 1;
	coprocessor_manager_note_banner();
	zassert_equal(status().generation, 2, "at the end of the window it is a restart");
	zassert_equal(status().unexpected_resets, 1);
	assert_marker(0, LOG_STORE_KIND_RESET, 2, "ESP32 restarted by itself");
}

ZTEST(coprocessor_manager, test_a_banner_after_a_reset_of_ours_is_expected)
{
	fake_uart.now_ms += 60000;
	zassert_ok(coprocessor_manager_reset(false));
	fake_uart.now_ms += 1000;
	coprocessor_manager_note_banner();
	zassert_equal(status().generation, 2);
	zassert_equal(status().unexpected_resets, 0);

	fake_uart.now_ms += WINDOW_MS;
	coprocessor_manager_note_banner();
	zassert_equal(status().generation, 3, "a watchdog restart later still counts");
	zassert_equal(status().unexpected_resets, 1);
}

/* --- the quiet wait, in detail (closing mutation survivors) -------------- */

/* Re-armed on every sleep: undoes the sleep's step, so the clock stands still. */
static void freeze_clock(void *arg)
{
	ARG_UNUSED(arg);
	fake_uart.now_ms -= STEP_MS;
	fake_uart.during_call = freeze_clock;
}

/*
 * The wait is bounded by looks as well as by the clock, so a clock that does
 * not move cannot turn a noisy UART into a hang - and the bound leaves room for
 * the two quiet looks after the last noisy one.
 */
ZTEST(coprocessor_manager, test_a_stuck_clock_still_ends_the_wait_after_a_bounded_number_of_looks)
{
	zassert_equal(STOP_TIMEOUT_MS % STEP_MS, 0, "the arithmetic below assumes whole steps");

	fake_uart.during_call = freeze_clock;
	fake_uart.noisy_reads = FAKE_UART_NEVER_QUIET;
	zassert_equal(coprocessor_manager_set_mode(BRIDGE), -ETIMEDOUT);
	zassert_equal(fake_uart.sleeps, STOP_TIMEOUT_MS / STEP_MS + 2);
	zassert_equal(status().uart_mode, CONSOLE);

	/* Noise until the timeout's worth of looks, then the two quiet looks. */
	fake_uart_init();
	zassert_ok(coprocessor_manager_init(&fake_uart_platform));
	fake_uart.during_call = freeze_clock;
	fake_uart.noisy_reads = STOP_TIMEOUT_MS / STEP_MS + 1;
	zassert_ok(coprocessor_manager_set_mode(BRIDGE), "quiet within the bounded looks");
	zassert_equal(status().uart_mode, BRIDGE);
}

static unsigned int bump_calls;

/* On the second sleep one byte arrives, then the line is quiet again. */
static void one_byte_on_the_second_look(void *arg)
{
	ARG_UNUSED(arg);
	if (++bump_calls == 2) {
		fake_uart.activity++;
	} else {
		fake_uart.during_call = one_byte_on_the_second_look;
	}
}

ZTEST(coprocessor_manager, test_a_byte_between_quiet_looks_starts_the_count_again)
{
	const int64_t start = fake_uart.now_ms;

	bump_calls = 0;
	fake_uart.during_call = one_byte_on_the_second_look;
	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	/* quiet, byte, quiet, quiet: two quiet looks after the byte, not one. */
	zassert_equal(fake_uart.now_ms - start, 4 * STEP_MS);
}

ZTEST(coprocessor_manager, test_a_noisy_interrupt_is_given_up_exactly_at_the_timeout)
{
	const int64_t start = fake_uart.now_ms;

	zassert_equal(STOP_TIMEOUT_MS % STEP_MS, 0);
	fake_uart.noisy_reads = FAKE_UART_NEVER_QUIET;
	zassert_equal(coprocessor_manager_set_mode(FLASHING), -ETIMEDOUT);
	zassert_equal(fake_uart.now_ms - start, STOP_TIMEOUT_MS);
}

ZTEST(coprocessor_manager, test_a_failed_stop_from_unavailable_asks_nobody_to_attach)
{
	start_in(UNAVAILABLE);
	fake_uart.noisy_reads = FAKE_UART_NEVER_QUIET;

	const unsigned int attaches = fake_uart.attach_calls;

	zassert_equal(coprocessor_manager_set_mode(BRIDGE), -ETIMEDOUT);
	zassert_equal(fake_uart.attach_calls, attaches, "there is no old owner to give it back to");
	zassert_equal(status().uart_mode, UNAVAILABLE);
	zassert_equal(fake_uart.marker_count, 0);
}

/* An unknown release must not touch anything, whatever lies next to the counts. */
ZTEST(coprocessor_manager, test_an_unknown_release_leaves_the_exclusion_alone)
{
	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	coprocessor_manager_release(COPROCESSOR_CLAIM_COUNT);
	coprocessor_manager_release((enum coprocessor_claim)-1);
	zassert_equal(coprocessor_manager_claim(APPLY), -EBUSY, "the bridge still excludes an apply");
}

/* --- readers do not wait ------------------------------------------------ */

K_THREAD_STACK_DEFINE(switch_stack, 2048);
static struct k_thread switch_thread;
static int switch_rc;

static void run_switch(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	switch_rc = coprocessor_manager_set_mode(BRIDGE);
}

/*
 * The HTTP handler that reads the status must not stop the server for as long
 * as a switch sleeps waiting for the interrupt to go quiet.
 */
ZTEST(coprocessor_manager, test_status_is_read_while_a_switch_sleeps)
{
	fake_uart_init();
	/* Long before now, so a banner would count if it were not ignored. */
	fake_uart.now_ms = -1000000;
	zassert_ok(coprocessor_manager_init(&fake_uart_platform));
	fake_uart.real_time = true;
	fake_uart.noisy_reads = FAKE_UART_NEVER_QUIET;

	switch_rc = 1;
	k_thread_create(&switch_thread, switch_stack, K_THREAD_STACK_SIZEOF(switch_stack),
			run_switch, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	k_msleep(STOP_TIMEOUT_MS / 4);
	zassert_equal(switch_rc, 1, "the switch is still sleeping");

	const int64_t before = k_uptime_get();
	struct coprocessor_status s;

	coprocessor_manager_get_status(&s);
	zassert_true(k_uptime_get() - before < STEP_MS, "the reader did not wait");
	zassert_equal(s.uart_mode, CONSOLE, "and saw the state before the switch");

	coprocessor_manager_note_banner();

	zassert_ok(k_thread_join(&switch_thread, K_SECONDS(2)));
	zassert_equal(switch_rc, -ETIMEDOUT);
	zassert_equal(status().generation, 1,
		      "a banner while a switch held the manager was ignored, not queued");
	zassert_equal(status().unexpected_resets, 0);
}

/* --- wire names --------------------------------------------------------- */

ZTEST(coprocessor_manager, test_wire_names_match_the_contract)
{
	zassert_str_equal(coprocessor_uart_mode_str(CONSOLE), "console");
	zassert_str_equal(coprocessor_uart_mode_str(BRIDGE), "usb_bridge");
	zassert_str_equal(coprocessor_uart_mode_str(FLASHING), "flashing");
	zassert_str_equal(coprocessor_uart_mode_str(UNAVAILABLE), "unavailable");
	zassert_is_null(coprocessor_uart_mode_str(COPROCESSOR_UART_MODE_COUNT));

	zassert_is_null(coprocessor_logs_unavailable_reason(CONSOLE));
	zassert_str_equal(coprocessor_logs_unavailable_reason(BRIDGE), "uart_usb_bridge");
	zassert_str_equal(coprocessor_logs_unavailable_reason(FLASHING), "uart_flashing");
	zassert_str_equal(coprocessor_logs_unavailable_reason(UNAVAILABLE), "uart_unavailable");
}
