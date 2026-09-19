/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board's side of Zephyr's coredump (prj.conf, owner's decisions 2026-09-18):
 * THREADS level into coredump_partition in the internal flash (board overlay).
 *
 * - The web API reads, streams and clears the stored dump through the hooks
 *   below (web_api_v1_set_coredump(); GET /api/v1/system/coredump and /data).
 * - A dump found at boot is logged, so it shows in the logs export too.
 * - assert_post_action() is replaced, see there: without it the dump is never
 *   written with CONFIG_ASSERT=y. It returns, which needs CONFIG_ASSERT_TEST=y.
 * - `crash fault|oops|panic` crashes on purpose, to check the whole chain on a
 *   board: dump, reset, GET /api/v1/system/coredump.
 *
 * Decoding on a PC, with the ELF of the image that crashed:
 *   zephyr/scripts/coredump/coredump_gdbserver.py zephyr.elf cedar-coredump.bin
 *   arm-zephyr-eabi-gdb zephyr.elf -ex "target remote localhost:1234"
 */

#include <errno.h>
#include <string.h>

#include <zephyr/debug/coredump.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/__assert.h>

#include <cmsis_core.h>

#include "web/api/v1/web_api_v1.h"

LOG_MODULE_REGISTER(coredump_support, LOG_LEVEL_INF);

/* -- the fault path -------------------------------------------------------------- */

/*
 * The kernel writes the dump from the fault handler (z_fatal_error() ->
 * coredump()), and the flash backend reaches the internal flash through
 * flash_stm32, which takes its semaphore with K_FOREVER - and k_sem_take()
 * asserts that an ISR does not wait. From an exception handler that assert
 * fires; the default action is k_panic(), a nested fatal error, and no dump.
 *
 * So while an exception handler runs - HardFault, MemManage, BusFault,
 * UsageFault (IPSR 3-6), or SVC (11), through which k_panic() and k_oops()
 * enter the same path - a failed assertion is reported and ignored. The fatal
 * path is already running there; a second one could only lose the dump. The
 * driver waits for the flash by polling, so it needs no interrupt. The
 * semaphore is free: nothing else writes this flash while the application
 * runs. (Were it taken, k_sem_take() would try to pend from the handler and the
 * dump would be lost.)
 *
 * Returning is only valid with CONFIG_ASSERT_TEST=y: otherwise __ASSERT() marks
 * the code after this call unreachable, and a return runs into whatever the
 * compiler put there - k_sem_take() fell into its pend path and then into a
 * literal pool, a second fault, no dump, IWDG reset (seen on the board
 * 2026-09-19).
 *
 * Everywhere else an assertion panics, as Zephyr's default does.
 */
#if defined(CONFIG_ASSERT) && !defined(CONFIG_ASSERT_TEST)
#error "assert_post_action() returns: needs CONFIG_ASSERT_TEST=y"
#endif
#if defined(CONFIG_ASSERT_NO_FILE_INFO)
void assert_post_action(void)
#else
void assert_post_action(const char *file, unsigned int line)
#endif
{
	const uint32_t ipsr = __get_IPSR();

#if !defined(CONFIG_ASSERT_NO_FILE_INFO)
	ARG_UNUSED(file);
	ARG_UNUSED(line);
#endif

	if ((ipsr >= 3U && ipsr <= 6U) || ipsr == 11U) {
		return;
	}
	k_panic();
}

/* -- the stored dump ---------------------------------------------------------------- */

static int dump_size(void)
{
	/* HAS_STORED_DUMP checks the checksum: a dump cut short by a reset in the
	 * middle of writing it is not offered. */
	int rc = coredump_query(COREDUMP_QUERY_HAS_STORED_DUMP, NULL);

	if (rc <= 0) {
		return rc;
	}
	return coredump_query(COREDUMP_QUERY_GET_STORED_DUMP_SIZE, NULL);
}

static int dump_read(size_t offset, uint8_t *buf, size_t len)
{
	struct coredump_cmd_copy_arg arg = {
		.offset = (off_t)offset,
		.buffer = buf,
		.length = len,
	};

	return coredump_cmd(COREDUMP_CMD_COPY_STORED_DUMP, &arg);
}

static int dump_clear(void)
{
	/* The header's page only (8 KiB): the next dump erases the whole partition. */
	int rc = coredump_cmd(COREDUMP_CMD_INVALIDATE_STORED_DUMP, NULL);

	return rc < 0 ? rc : 0;
}

static const struct web_api_v1_coredump dump_hooks = {
	.size = dump_size,
	.read = dump_read,
	.clear = dump_clear,
};

static int coredump_support_init(void)
{
	int size = dump_size();

	web_api_v1_set_coredump(&dump_hooks);

	if (size > 0) {
		uint8_t hdr[sizeof(struct coredump_hdr_t)];

		if (dump_read(0, hdr, sizeof(hdr)) == (int)sizeof(hdr)) {
			const struct coredump_hdr_t *h = (const struct coredump_hdr_t *)hdr;

			LOG_ERR("coredump stored: %d bytes, reason %u; GET /api/v1/system/coredump/data "
				"or `coredump print`",
				size, h->reason);
		} else {
			LOG_ERR("coredump stored: %d bytes", size);
		}
	} else if (size < 0) {
		LOG_WRN("coredump partition unreadable: %d", size);
	}
	return 0;
}

SYS_INIT(coredump_support_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* -- crashing on purpose (debug shell) ------------------------------------------------ */

/* Out of line so the dump shows a caller. An undefined instruction: a UsageFault
 * on every Cortex-M, wherever the memory map puts things. */
static __noinline void fault_now(void)
{
	__builtin_trap();
}

static int cmd_crash(const struct shell *sh, size_t argc, char **argv)
{
	const char *kind = argc > 1 ? argv[1] : "fault";

	shell_print(sh, "crashing on purpose (%s): coredump, then reset", kind);
	k_msleep(100);
	if (strcmp(kind, "oops") == 0) {
		k_oops();
	} else if (strcmp(kind, "panic") == 0) {
		k_panic();
	} else {
		fault_now();
	}
	return 0;
}

SHELL_CMD_ARG_REGISTER(crash, NULL, "Crash now to test the coredump: [fault|oops|panic]", cmd_crash,
		       1, 1);
