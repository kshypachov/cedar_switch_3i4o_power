/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp-loader-adapter unit tests.
 *
 * Section 12 asks this tier for the translation of the library's errors, the
 * progress, the order in which the UART is taken and returned, and GPIO
 * cleanup with a normal boot on every exit, emergency ones included. The
 * library is tests/fakes/fake_esp_loader.c, which records each call with the
 * UART owner at that moment; coprocessor-manager is the real one on the fake
 * UART, so "the UART is the flasher's" is the manager's own answer.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <esp_loader_adapter/esp_loader_adapter.h>
#include <esp_loader_adapter/updater_loader.h>

#include "fake_esp_loader.h"
#include "fake_uart.h"

#define CONSOLE  COPROCESSOR_UART_CONSOLE
#define BRIDGE   COPROCESSOR_UART_USB_BRIDGE
#define FLASHING COPROCESSOR_UART_FLASHING
#define BLOCK    CONFIG_ESP_LOADER_ADAPTER_BLOCK_SIZE
#define HIGH     CONFIG_ESP_LOADER_ADAPTER_HIGH_BAUD

static void case_before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_uart_init();
	fake_uart.now_ms = 10000;
	zassert_ok(coprocessor_manager_init(&fake_uart_platform));
	fake_esp_loader_init();
	zassert_ok(esp_loader_adapter_init(&fake_esp_loader_lib));
}

static void case_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)esp_loader_adapter_close();
}

ZTEST_SUITE(esp_loader_adapter, NULL, NULL, case_before, case_after, NULL);

/* --- helpers ------------------------------------------------------------ */

static enum coprocessor_uart_mode mode(void)
{
	struct coprocessor_status s;

	coprocessor_manager_get_status(&s);
	return s.uart_mode;
}

static uint8_t image_byte(uint32_t i)
{
	return (uint8_t)(i * 13U + 1U);
}

/* Write @p size pattern bytes in pieces of @p piece. */
static int write_image(uint32_t size, size_t piece, struct esp_loader_adapter_error *err)
{
	uint8_t buf[3000];

	zassert_true(piece <= sizeof(buf));
	for (uint32_t off = 0; off < size;) {
		size_t n = MIN(piece, (size_t)(size - off));

		for (size_t i = 0; i < n; i++) {
			buf[i] = image_byte(off + (uint32_t)i);
		}
		int rc = esp_loader_adapter_write(buf, n, err);

		if (rc != 0) {
			return rc;
		}
		off += (uint32_t)n;
	}
	return 0;
}

/* The steps of close, in order, after position @p from; and the console back. */
static void assert_cleanup_after(int from, bool port_was_up)
{
	int reset = fake_esp_loader_last(FEL_RESET_TARGET);
	int deinit = fake_esp_loader_last(FEL_PORT_DEINIT);
	int idle = fake_esp_loader_last(FEL_LINES_IDLE);
	int restore = fake_esp_loader_last(FEL_CONSOLE_RESTORE);

	if (port_was_up) {
		zassert_equal(fake_esp_loader.count[FEL_PORT_DEINIT], fake_esp_loader.count[FEL_PORT_INIT],
			      "every port initialised was deinitialised");
		zassert_true(reset > from, "normal boot after the failure");
		zassert_true(deinit > reset, "normal boot needs the port: before deinit");
		zassert_true(idle > deinit, "EN/BOOT back after deinit left them floating");
	} else {
		zassert_equal(reset, -1, "no port, no library call");
		zassert_equal(deinit, -1);
		zassert_true(idle > from);
	}
	zassert_true(restore > idle, "the console's configuration before the console");
	if (from >= 0) {
		/* After a switch that failed the manager already put the console back. */
		zassert_equal(fake_esp_loader.mode_at[FEL_CONSOLE_RESTORE], FLASHING,
			      "still the flasher's while its settings are undone");
	}
	zassert_equal(mode(), CONSOLE, "the console has the UART again");
	zassert_true(fake_uart.handler, "the console's handler is installed");
	zassert_false(fake_esp_loader.port_live);
	zassert_false(esp_loader_adapter_is_open());
}

/* --- init --------------------------------------------------------------- */

#define WITHOUT(field)                                                                             \
	do {                                                                                       \
		struct esp_loader_adapter_lib lib = fake_esp_loader_lib;                           \
                                                                                                   \
		lib.field = NULL;                                                                  \
		zassert_equal(esp_loader_adapter_init(&lib), -EINVAL, #field);                     \
	} while (0)

ZTEST(esp_loader_adapter, test_init_requires_every_call_but_change_rate)
{
	struct esp_loader_adapter_lib lib = fake_esp_loader_lib;

	zassert_equal(esp_loader_adapter_init(NULL), -EINVAL);
	WITHOUT(port_init);
	WITHOUT(port_deinit);
	WITHOUT(connect);
	WITHOUT(get_target);
	WITHOUT(flash_start);
	WITHOUT(flash_write);
	WITHOUT(flash_finish);
	WITHOUT(reset_target);
	WITHOUT(lines_idle);
	WITHOUT(console_restore);
	lib.change_rate = NULL;
	zassert_ok(esp_loader_adapter_init(&lib));
}

ZTEST(esp_loader_adapter, test_init_refused_while_a_session_is_open)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_equal(esp_loader_adapter_init(&fake_esp_loader_lib), -EBUSY);
}

/* --- open --------------------------------------------------------------- */

ZTEST(esp_loader_adapter, test_open_takes_the_uart_before_the_library_touches_it)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_true(esp_loader_adapter_is_open());
	zassert_equal(mode(), FLASHING);
	zassert_false(fake_uart.handler, "no log handler shares the UART with the flasher");

	zassert_equal(fake_esp_loader.calls[0], FEL_PORT_INIT);
	zassert_equal(fake_esp_loader.calls[1], FEL_CONNECT);
	zassert_equal(fake_esp_loader.calls[2], FEL_GET_TARGET);
	zassert_equal(fake_esp_loader.mode_at[FEL_PORT_INIT], FLASHING,
		      "the manager handed the UART over first");
	zassert_equal(fake_esp_loader.count[FEL_CHANGE_RATE], HIGH > 0 ? 1 : 0);
	if (HIGH > 0) {
		zassert_equal(fake_esp_loader.calls[3], FEL_CHANGE_RATE, "after the chip is known");
		zassert_equal(fake_esp_loader.rate, HIGH);
	}
	zassert_equal(fake_esp_loader.count[FEL_FLASH_START], 0, "open erases nothing");
}

ZTEST(esp_loader_adapter, test_open_refused_while_the_bridge_has_the_uart_touches_nothing)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(coprocessor_manager_set_mode(BRIDGE));
	zassert_equal(esp_loader_adapter_open(&err), -EBUSY);
	zassert_str_equal(err.code, "busy");
	zassert_true(err.retryable);
	zassert_equal(fake_esp_loader.call_count, 0,
		      "not even the lines: the bridge's host may be using the C6");
	zassert_equal(mode(), BRIDGE);
	zassert_false(esp_loader_adapter_is_open());
}

ZTEST(esp_loader_adapter, test_open_refused_during_a_network_apply)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(coprocessor_manager_claim(COPROCESSOR_CLAIM_NETWORK_APPLY));
	zassert_equal(esp_loader_adapter_open(&err), -EBUSY);
	zassert_str_equal(err.code, "busy");
	zassert_equal(fake_esp_loader.call_count, 0);
	zassert_equal(mode(), CONSOLE);
	coprocessor_manager_release(COPROCESSOR_CLAIM_NETWORK_APPLY);
}

ZTEST(esp_loader_adapter, test_open_while_open_is_busy)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	fake_esp_loader_init();
	zassert_equal(esp_loader_adapter_open(&err), -EBUSY);
	zassert_str_equal(err.code, "busy");
	zassert_true(err.retryable);
	zassert_equal(fake_esp_loader.call_count, 0);
	zassert_true(esp_loader_adapter_is_open(), "the first session is untouched");
	zassert_equal(mode(), FLASHING);
}

ZTEST(esp_loader_adapter, test_uart_that_will_not_go_quiet_stays_with_the_console)
{
	struct esp_loader_adapter_error err = {0};

	fake_uart.noisy_reads = FAKE_UART_NEVER_QUIET;
	zassert_equal(esp_loader_adapter_open(&err), -EIO);
	zassert_str_equal(err.code, "internal_error");
	zassert_equal(err.cause, -ETIMEDOUT);
	zassert_true(err.retryable);
	zassert_equal(fake_esp_loader.call_count, 0, "the console still owns the UART");
	zassert_equal(mode(), CONSOLE);
	zassert_false(esp_loader_adapter_is_open());
}

ZTEST(esp_loader_adapter, test_failed_switch_still_puts_lines_and_console_back)
{
	struct esp_loader_adapter_error err = {0};

	fake_uart.attach_errno[FLASHING] = -EIO;
	zassert_equal(esp_loader_adapter_open(&err), -EIO);
	zassert_str_equal(err.code, "internal_error");
	zassert_true(err.retryable);
	zassert_equal(err.cause, -EIO);
	zassert_equal(fake_esp_loader.count[FEL_PORT_INIT], 0);
	assert_cleanup_after(-1, false);
}

ZTEST(esp_loader_adapter, test_port_init_failure_cleans_up_without_library_calls)
{
	struct esp_loader_adapter_error err = {0};

	fake_esp_loader.result[FEL_PORT_INIT] = 1;
	zassert_equal(esp_loader_adapter_open(&err), -EIO);
	zassert_str_equal(err.code, "internal_error");
	zassert_true(err.retryable);
	zassert_equal(err.cause, 1);
	zassert_equal(fake_esp_loader.count[FEL_CONNECT], 0);
	assert_cleanup_after(fake_esp_loader_first(FEL_PORT_INIT), false);
}

ZTEST(esp_loader_adapter, test_rom_loader_silence_is_service_not_ready_and_boots_normally)
{
	struct esp_loader_adapter_error err = {0};

	fake_esp_loader.result[FEL_CONNECT] = ESP_LOADER_ADAPTER_LIB_TIMEOUT;
	zassert_equal(esp_loader_adapter_open(&err), -EIO);
	zassert_str_equal(err.code, "service_not_ready");
	zassert_true(err.retryable);
	zassert_equal(err.cause, ESP_LOADER_ADAPTER_LIB_TIMEOUT);
	zassert_not_null(strstr(err.message, "ROM loader"));
	/* The straps may have left the C6 in download mode. */
	assert_cleanup_after(fake_esp_loader_first(FEL_CONNECT), true);
}

ZTEST(esp_loader_adapter, test_rom_loader_nonsense_is_internal_error)
{
	struct esp_loader_adapter_error err = {0};

	fake_esp_loader.result[FEL_CONNECT] = 9; /* ESP_LOADER_ERROR_INVALID_RESPONSE */
	zassert_equal(esp_loader_adapter_open(&err), -EIO);
	zassert_str_equal(err.code, "internal_error");
	zassert_equal(err.cause, 9);
	assert_cleanup_after(fake_esp_loader_first(FEL_CONNECT), true);
}

ZTEST(esp_loader_adapter, test_other_chip_is_unsupported_target_and_not_retryable)
{
	struct esp_loader_adapter_error err = {0};

	fake_esp_loader.target = 5; /* ESP32C2_CHIP */
	zassert_equal(esp_loader_adapter_open(&err), -ENOTSUP);
	zassert_str_equal(err.code, "unsupported_target");
	zassert_false(err.retryable);
	zassert_equal(err.cause, 5);
	zassert_equal(fake_esp_loader.count[FEL_FLASH_START], 0, "nothing erased on a stranger");
	zassert_equal(fake_esp_loader.count[FEL_CHANGE_RATE], 0);
	assert_cleanup_after(fake_esp_loader_first(FEL_GET_TARGET), true);
}

ZTEST(esp_loader_adapter, test_refused_higher_rate_cleans_up)
{
	struct esp_loader_adapter_error err = {0};

	if (HIGH == 0) {
		ztest_test_skip();
	}
	fake_esp_loader.result[FEL_CHANGE_RATE] = 1;
	zassert_equal(esp_loader_adapter_open(&err), -EIO);
	zassert_str_equal(err.code, "internal_error");
	zassert_true(err.retryable);
	zassert_equal(fake_esp_loader.count[FEL_FLASH_START], 0);
	assert_cleanup_after(fake_esp_loader_first(FEL_CHANGE_RATE), true);
}

/* --- write -------------------------------------------------------------- */

ZTEST(esp_loader_adapter, test_image_is_written_from_zero_padded_to_four_and_verified)
{
	struct esp_loader_adapter_error err = {0};
	const uint32_t size = 2 * BLOCK + 5; /* 4 bytes into a block, rounds up by 3 */

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_begin(size, &err));
	zassert_equal(fake_esp_loader.start_offset, 0x0, "always 0x0");
	zassert_equal(fake_esp_loader.start_size, ROUND_UP(size, 4));
	zassert_equal(fake_esp_loader.start_block, BLOCK);

	zassert_ok(write_image(size, 700, &err));
	zassert_equal(esp_loader_adapter_written(), size);
	zassert_equal(fake_esp_loader.count[FEL_FLASH_WRITE], 2, "whole blocks only until finish");
	zassert_ok(esp_loader_adapter_finish(&err));
	zassert_equal(fake_esp_loader.count[FEL_FLASH_WRITE], 3);
	zassert_equal(fake_esp_loader.count[FEL_FLASH_FINISH], 1);
	zassert_true(fake_esp_loader_last(FEL_FLASH_FINISH) > fake_esp_loader_last(FEL_FLASH_WRITE));
	zassert_equal(fake_esp_loader.largest_write, BLOCK);

	zassert_equal(fake_esp_loader.written, ROUND_UP(size, 4));
	for (uint32_t i = 0; i < size; i++) {
		zassert_equal(fake_esp_loader.bytes[i], image_byte(i), "byte %u", i);
	}
	for (uint32_t i = size; i < ROUND_UP(size, 4); i++) {
		zassert_equal(fake_esp_loader.bytes[i], 0xFF, "padding is erased flash");
	}

	zassert_ok(esp_loader_adapter_close());
	assert_cleanup_after(fake_esp_loader_last(FEL_FLASH_FINISH), true);
}

ZTEST(esp_loader_adapter, test_image_of_whole_blocks_sends_no_empty_block)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_begin(2 * BLOCK, &err));
	zassert_ok(write_image(2 * BLOCK, BLOCK, &err));
	zassert_ok(esp_loader_adapter_finish(&err));
	zassert_equal(fake_esp_loader.count[FEL_FLASH_WRITE], 2);
	zassert_equal(fake_esp_loader.written, 2 * BLOCK);
}

ZTEST(esp_loader_adapter, test_padding_to_four_fills_the_last_block)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_begin(BLOCK - 1, &err));
	zassert_ok(write_image(BLOCK - 1, 100, &err));
	zassert_ok(esp_loader_adapter_finish(&err));
	zassert_equal(fake_esp_loader.count[FEL_FLASH_WRITE], 1);
	zassert_equal(fake_esp_loader.written, BLOCK);
	zassert_equal(fake_esp_loader.bytes[BLOCK - 1], 0xFF);
}

/* A refusal of a call out of order: a caller's mistake, reported, not retryable. */
static void assert_misuse(int rc, int expected, const struct esp_loader_adapter_error *err)
{
	zassert_equal(rc, expected);
	zassert_not_null(err->code, "the error is filled in");
	zassert_str_equal(err->code, "internal_error");
	zassert_not_null(err->message);
	zassert_false(err->retryable, "repeating the same call cannot help");
	zassert_equal(err->cause, expected);
}

ZTEST(esp_loader_adapter, test_order_rules_of_the_write)
{
	struct esp_loader_adapter_error err = {0};
	uint8_t byte = 0;

	/* No session. */
	err = (struct esp_loader_adapter_error){0};
	assert_misuse(esp_loader_adapter_begin(4, &err), -EPERM, &err);
	err = (struct esp_loader_adapter_error){0};
	assert_misuse(esp_loader_adapter_write(&byte, 1, &err), -EPERM, &err);
	err = (struct esp_loader_adapter_error){0};
	assert_misuse(esp_loader_adapter_finish(&err), -EPERM, &err);

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_equal(esp_loader_adapter_write(&byte, 1, &err), -EPERM, "before begin");
	zassert_equal(esp_loader_adapter_finish(&err), -EPERM, "before begin");
	err = (struct esp_loader_adapter_error){0};
	assert_misuse(esp_loader_adapter_begin(0, &err), -EINVAL, &err);
	zassert_equal(fake_esp_loader.count[FEL_FLASH_START], 0);

	zassert_ok(esp_loader_adapter_begin(8, &err));
	zassert_equal(esp_loader_adapter_begin(8, &err), -EPERM, "one write at a time");
	zassert_ok(write_image(7, 7, &err));
	zassert_equal(esp_loader_adapter_finish(&err), -EINVAL, "one byte short");
	zassert_equal(fake_esp_loader.count[FEL_FLASH_FINISH], 0);
	zassert_ok(write_image(1, 1, &err), "the write goes on after a short finish");
	err = (struct esp_loader_adapter_error){0};
	assert_misuse(esp_loader_adapter_write(&byte, 1, &err), -EINVAL, &err);
	zassert_equal(esp_loader_adapter_written(), 8);
	zassert_ok(esp_loader_adapter_finish(&err));
	zassert_equal(esp_loader_adapter_finish(&err), -EPERM, "finished");
}

ZTEST(esp_loader_adapter, test_refused_begin_closes_the_session_at_once)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	fake_esp_loader.result[FEL_FLASH_START] = ESP_LOADER_ADAPTER_LIB_TIMEOUT;
	zassert_equal(esp_loader_adapter_begin(100, &err), -EIO);
	zassert_str_equal(err.code, "internal_error");
	zassert_true(err.retryable);
	zassert_equal(err.cause, ESP_LOADER_ADAPTER_LIB_TIMEOUT);
	assert_cleanup_after(fake_esp_loader_first(FEL_FLASH_START), true);
	zassert_equal(esp_loader_adapter_write((const uint8_t *)"x", 1, &err), -EPERM);
	zassert_ok(esp_loader_adapter_close(), "nothing left to close");
	zassert_equal(fake_esp_loader.count[FEL_LINES_IDLE], 1, "and nothing done twice");
}

ZTEST(esp_loader_adapter, test_block_refused_in_the_middle_ends_the_write_and_cleans_up)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_begin(3 * BLOCK, &err));
	fake_esp_loader.result[FEL_FLASH_WRITE] = ESP_LOADER_ADAPTER_LIB_TIMEOUT;
	fake_esp_loader.write_fail_at = 2;
	zassert_equal(write_image(3 * BLOCK, BLOCK, &err), -EIO);
	zassert_str_equal(err.code, "internal_error");
	zassert_true(err.retryable);
	zassert_equal(err.cause, ESP_LOADER_ADAPTER_LIB_TIMEOUT);
	/* Torn down before returning: no initialised port waits for the caller. */
	assert_cleanup_after(fake_esp_loader_last(FEL_FLASH_WRITE), true);
	zassert_equal(esp_loader_adapter_write((const uint8_t *)"x", 1, &err), -EPERM,
		      "the ROM's sequence is broken");
	zassert_equal(esp_loader_adapter_finish(&err), -EPERM);
	zassert_equal(fake_esp_loader.count[FEL_FLASH_FINISH], 0);
	zassert_ok(esp_loader_adapter_close());
}

ZTEST(esp_loader_adapter, test_last_block_refused_at_finish)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_begin(10, &err));
	zassert_ok(write_image(10, 10, &err));
	fake_esp_loader.result[FEL_FLASH_WRITE] = 1;
	zassert_equal(esp_loader_adapter_finish(&err), -EIO);
	zassert_equal(fake_esp_loader.count[FEL_FLASH_FINISH], 0, "no MD5 of a missing block");
	assert_cleanup_after(fake_esp_loader_last(FEL_FLASH_WRITE), true);
	zassert_equal(esp_loader_adapter_finish(&err), -EPERM);
}

ZTEST(esp_loader_adapter, test_md5_mismatch_is_reported_as_such)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_begin(12, &err));
	zassert_ok(write_image(12, 12, &err));
	fake_esp_loader.result[FEL_FLASH_FINISH] = ESP_LOADER_ADAPTER_LIB_INVALID_MD5;
	zassert_equal(esp_loader_adapter_finish(&err), -EIO);
	zassert_str_equal(err.code, "internal_error");
	zassert_true(err.retryable);
	zassert_not_null(strstr(err.message, "MD5"));
	assert_cleanup_after(fake_esp_loader_last(FEL_FLASH_FINISH), true);
	zassert_equal(esp_loader_adapter_finish(&err), -EPERM);
	zassert_ok(esp_loader_adapter_close());
}

ZTEST(esp_loader_adapter, test_other_finish_failure_is_not_called_md5)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_begin(4, &err));
	zassert_ok(write_image(4, 4, &err));
	fake_esp_loader.result[FEL_FLASH_FINISH] = ESP_LOADER_ADAPTER_LIB_TIMEOUT;
	zassert_equal(esp_loader_adapter_finish(&err), -EIO);
	zassert_is_null(strstr(err.message, "MD5"));
	assert_cleanup_after(fake_esp_loader_last(FEL_FLASH_FINISH), true);
}

/*
 * tty's receive handler, installed by the port init, blocks its ISR forever when
 * its ring overflows with nobody reading (reports/p6, hw/logs/02). Whatever
 * library call fails, the port must be down and the console back when the call
 * returns - not only after the caller gets round to close().
 */
ZTEST(esp_loader_adapter, test_no_failure_leaves_the_port_initialised)
{
	enum point { CONNECT, TARGET, BEGIN, WRITE, LAST_BLOCK, FINISH, POINTS };

	for (int p = 0; p < POINTS; p++) {
		struct esp_loader_adapter_error err = {0};
		int rc = 0;

		case_before(NULL);
		switch (p) {
		case CONNECT:
			fake_esp_loader.result[FEL_CONNECT] = 1;
			break;
		case TARGET:
			fake_esp_loader.target = 0;
			break;
		case BEGIN:
			fake_esp_loader.result[FEL_FLASH_START] = 1;
			break;
		case WRITE:
			fake_esp_loader.result[FEL_FLASH_WRITE] = 1;
			fake_esp_loader.write_fail_at = 1;
			break;
		case LAST_BLOCK:
			fake_esp_loader.result[FEL_FLASH_WRITE] = 1;
			fake_esp_loader.write_fail_at = 2;
			break;
		default:
			fake_esp_loader.result[FEL_FLASH_FINISH] = 1;
			break;
		}
		rc = esp_loader_adapter_open(&err);
		if (rc == 0) {
			rc = esp_loader_adapter_begin(BLOCK + 8, &err);
		}
		if (rc == 0) {
			rc = write_image(BLOCK + 8, BLOCK + 8, &err);
		}
		if (rc == 0) {
			rc = esp_loader_adapter_finish(&err);
		}
		zassert_not_ok(rc, "point %d fails", p);
		zassert_false(fake_esp_loader.port_live, "point %d: port down on return", p);
		zassert_equal(fake_esp_loader.count[FEL_PORT_DEINIT], 1, "point %d", p);
		zassert_true(fake_esp_loader_first(FEL_PORT_INIT) >= 0);
		zassert_equal(fake_esp_loader.mode_at[FEL_PORT_INIT], FLASHING,
			      "point %d: init only after the console was detached", p);
		zassert_equal(mode(), CONSOLE, "point %d: console back on return", p);
		zassert_false(esp_loader_adapter_is_open(), "point %d", p);
	}
}

/* --- close -------------------------------------------------------------- */

ZTEST(esp_loader_adapter, test_close_without_a_session_does_nothing)
{
	zassert_ok(esp_loader_adapter_close());
	zassert_equal(fake_esp_loader.call_count, 0);
	zassert_equal(mode(), CONSOLE);
}

ZTEST(esp_loader_adapter, test_close_runs_every_step_even_when_one_fails)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	fake_esp_loader.result[FEL_LINES_IDLE] = -EIO;
	fake_esp_loader.result[FEL_CONSOLE_RESTORE] = -ENODEV;
	zassert_equal(esp_loader_adapter_close(), -EIO, "the first failure is reported");
	assert_cleanup_after(fake_esp_loader_first(FEL_GET_TARGET), true);
	zassert_ok(esp_loader_adapter_close(), "and the session is over");
}

ZTEST(esp_loader_adapter, test_close_reports_a_console_that_will_not_attach)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	fake_esp_loader.result[FEL_CONSOLE_RESTORE] = -ENODEV;
	zassert_equal(esp_loader_adapter_close(), -ENODEV);

	zassert_ok(esp_loader_adapter_open(&err));
	fake_esp_loader_init();
	fake_uart.attach_errno[CONSOLE] = -EIO;
	zassert_equal(esp_loader_adapter_close(), -EIO, "the manager's refusal is reported");
	zassert_equal(fake_esp_loader.count[FEL_LINES_IDLE], 1);
	zassert_false(esp_loader_adapter_is_open());
	fake_uart.attach_errno[CONSOLE] = 0;
}

ZTEST(esp_loader_adapter, test_close_keeps_the_first_failure_when_the_console_also_fails)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	fake_esp_loader.result[FEL_CONSOLE_RESTORE] = -ENODEV;
	fake_uart.attach_errno[CONSOLE] = -EIO;
	zassert_equal(esp_loader_adapter_close(), -ENODEV, "the earlier step's failure is the one reported");
	fake_uart.attach_errno[CONSOLE] = 0;
}

ZTEST(esp_loader_adapter, test_a_finished_session_leaves_no_port_behind)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_close());
	fake_esp_loader_init();
	fake_esp_loader.result[FEL_PORT_INIT] = 1;
	zassert_equal(esp_loader_adapter_open(&err), -EIO);
	zassert_equal(fake_esp_loader.count[FEL_RESET_TARGET], 0,
		      "the port of the last session is not this session's");
	zassert_equal(fake_esp_loader.count[FEL_PORT_DEINIT], 0);
}

ZTEST(esp_loader_adapter, test_close_marks_a_new_generation_in_the_log)
{
	struct esp_loader_adapter_error err = {0};
	struct coprocessor_status before, after;

	coprocessor_manager_get_status(&before);
	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_close());
	coprocessor_manager_get_status(&after);
	zassert_equal(after.generation, before.generation + 1, "the C6 was reset under the log");
}

ZTEST(esp_loader_adapter, test_a_second_session_initialises_the_port_again)
{
	struct esp_loader_adapter_error err = {0};

	zassert_ok(esp_loader_adapter_open(&err));
	zassert_ok(esp_loader_adapter_close());
	zassert_ok(esp_loader_adapter_open(&err));
	zassert_equal(fake_esp_loader.count[FEL_PORT_INIT], 2);
	zassert_true(fake_esp_loader.port_live);
	zassert_equal(mode(), FLASHING);
}

/* --- the updater's ops -------------------------------------------------- */

ZTEST(esp_loader_adapter, test_updater_ops_pass_through_and_copy_errors)
{
	const struct coprocessor_updater_loader *ld = esp_loader_adapter_updater_loader();
	struct coprocessor_update_error err = {0};
	uint8_t data[6] = {1, 2, 3, 4, 5, 6};

	zassert_not_null(ld);
	/* A success leaves the caller's error alone. */
	err.code = "untouched";
	zassert_ok(ld->open(ld->ctx, &err));
	zassert_equal(mode(), FLASHING);
	zassert_ok(ld->begin(ld->ctx, sizeof(data), &err));
	zassert_ok(ld->write(ld->ctx, data, sizeof(data), &err));
	zassert_ok(ld->finish(ld->ctx, &err));
	zassert_str_equal(err.code, "untouched");
	zassert_equal(fake_esp_loader.written, 8);
	zassert_ok(ld->close(ld->ctx));
	zassert_equal(mode(), CONSOLE);

	fake_esp_loader.target = 1;
	zassert_equal(ld->open(ld->ctx, &err), -ENOTSUP);
	zassert_str_equal(err.code, "unsupported_target");
	zassert_false(err.retryable);
	zassert_not_null(err.message);

	fake_esp_loader_init();
	zassert_ok(ld->open(ld->ctx, &err));
	fake_esp_loader.result[FEL_FLASH_START] = 1;
	zassert_equal(ld->begin(ld->ctx, 4, &err), -EIO);
	zassert_str_equal(err.code, "internal_error");
	zassert_ok(ld->close(ld->ctx), "already closed by the failure");
	fake_esp_loader.result[FEL_FLASH_START] = 0;
	zassert_ok(ld->open(ld->ctx, &err));
	zassert_ok(ld->begin(ld->ctx, 4, &err));
	err = (struct coprocessor_update_error){0};
	zassert_equal(ld->write(ld->ctx, data, sizeof(data), &err), -EINVAL);
	zassert_str_equal(err.code, "internal_error", "a write's error is passed on");
	zassert_false(err.retryable);
	err = (struct coprocessor_update_error){0};
	zassert_equal(ld->finish(ld->ctx, &err), -EINVAL);
	zassert_str_equal(err.code, "internal_error", "a finish's error is passed on");
	zassert_not_null(err.message);
	err = (struct coprocessor_update_error){0};
	fake_esp_loader.result[FEL_FLASH_FINISH] = ESP_LOADER_ADAPTER_LIB_INVALID_MD5;
	zassert_ok(ld->write(ld->ctx, data, 4, &err));
	zassert_equal(ld->finish(ld->ctx, &err), -EIO);
	zassert_not_null(strstr(err.message, "MD5"), "the adapter's own message");
	zassert_true(err.retryable);
	zassert_ok(ld->close(ld->ctx));
}
