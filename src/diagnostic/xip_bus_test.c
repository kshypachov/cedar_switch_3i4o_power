/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Debug shell: program slot 2 of the OCTOSPI NOR the firmware executes from, page by
 * page, through the XIP-safe driver path, with the stack and the source in PSRAM or
 * in SRAM (reports/xip-watchdog).
 *
 *   xipbus psram <pages> [flush|nocache]   caller thread and source buffer in PSRAM
 *   xipbus sram <pages> [flush|nocache]    the shell thread (SRAM stack), source in SRAM
 *
 * flush cleans and invalidates DCACHE1 before every page. nocache cleans it and turns
 * it off for the whole run, so every PSRAM access of the guarded chunk, stack included,
 * goes to the bus (the rest of the firmware runs uncached meanwhile). The PSRAM is on OCTOSPI2, which
 * shares CLK/IO0..3 with OCTOSPI1 through OCTOSPIM in multiplexed mode: a PSRAM
 * access while OCTOSPI1 is in the data phase of a page program waits for a bus the
 * page program holds until the CPU feeds it the next byte.
 *
 * Erases the first sectors of slot 2 and writes a pattern there: whatever image was
 * staged in slot 2 is lost.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>

#define PAGE     256U
#define SECTOR   4096U
#define MAX_PAGES 1024U

/* Both in the app library: .bss, linked into PSRAM (CMakeLists.txt). */
static K_THREAD_STACK_DEFINE(psram_stack, 2048);
static uint8_t psram_src[PAGE];
static struct k_thread psram_thread;

struct run {
	const struct shell *sh;
	uint8_t *src;
	uint32_t pages;
	bool flush;
	bool nocache;
	int rc;
	uint32_t done;
	int64_t ms;
};

static void program(struct run *r)
{
	const struct flash_area *fa = NULL;
	int64_t t0 = k_uptime_get();
	int rc = flash_area_open(PARTITION_ID(slot1_partition), &fa);

	if (rc == 0) {
		rc = flash_area_erase(fa, 0, ROUND_UP(r->pages * PAGE, SECTOR));
	}
	for (uint32_t i = 0; rc == 0 && i < r->pages; i++) {
		memset(r->src, (int)(i & 0xFFU), PAGE);
		if (r->flush) {
			(void)sys_cache_data_flush_and_invd_all();
		}
		rc = flash_area_write(fa, i * PAGE, r->src, PAGE);
		r->done = i + 1U;
	}
	if (fa != NULL) {
		flash_area_close(fa);
	}
	r->rc = rc;
	r->ms = k_uptime_get() - t0;
}

static void psram_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	program(a);
}

static int cmd_run(const struct shell *sh, size_t argc, char **argv, bool in_psram)
{
	uint8_t sram_src[PAGE]; /* on the shell thread's stack, which is in SRAM */
	struct run r = {
		.sh = sh,
		.src = in_psram ? psram_src : sram_src,
		.pages = (uint32_t)strtoul(argv[1], NULL, 0),
		.flush = argc > 2 && strcmp(argv[2], "flush") == 0,
		.nocache = argc > 2 && strcmp(argv[2], "nocache") == 0,
	};
	int probe;

	if (r.pages == 0U || r.pages > MAX_PAGES) {
		shell_error(sh, "pages 1..%u", MAX_PAGES);
		return -EINVAL;
	}
	shell_print(sh, "%s: stack %p src %p, %u pages%s", in_psram ? "psram" : "sram",
		    in_psram ? (void *)psram_stack : (void *)&probe, (void *)r.src, r.pages,
		    r.flush ? ", DCACHE flushed before each" : (r.nocache ? ", DCACHE off" : ""));
	k_msleep(50);
	if (r.nocache) {
		(void)sys_cache_data_flush_all();
		(void)sys_cache_data_disable();
	}
	if (in_psram) {
		k_tid_t t = k_thread_create(&psram_thread, psram_stack,
					    K_THREAD_STACK_SIZEOF(psram_stack), psram_entry, &r,
					    NULL, NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);

		k_thread_name_set(t, "xipbus");
		k_thread_join(t, K_FOREVER);
	} else {
		program(&r);
	}
	if (r.nocache) {
		(void)sys_cache_data_invd_all();
		(void)sys_cache_data_enable();
	}
	shell_print(sh, "%u of %u pages in %lld ms: %d", r.done, r.pages, r.ms, r.rc);
	return r.rc;
}

static int cmd_psram(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_run(sh, argc, argv, true);
}

static int cmd_sram(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_run(sh, argc, argv, false);
}

SHELL_STATIC_SUBCMD_SET_CREATE(xipbus_cmds,
	SHELL_CMD_ARG(psram, NULL, "<pages> [flush|nocache]: stack and source in PSRAM", cmd_psram, 2, 1),
	SHELL_CMD_ARG(sram, NULL, "<pages> [flush|nocache]: stack and source in SRAM", cmd_sram, 2, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(xipbus, &xipbus_cmds, "XIP page program vs PSRAM bus (debug)", NULL);
