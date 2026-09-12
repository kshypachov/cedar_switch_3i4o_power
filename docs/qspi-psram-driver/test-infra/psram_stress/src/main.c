/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Full-volume PSRAM stress test over the memory-mapped window.
 *
 * Each cycle runs two passes: an address-derived pattern (every word holds
 * its own byte offset, so aliasing and addressing faults are visible) and
 * its bitwise inverse (covers both polarities of every bit). A pass fills
 * the ENTIRE memory in one continuous streaming loop, then verifies the
 * ENTIRE memory on the device. Only service lines and the final verdict go
 * to the console; the test never prints data dumps.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/memc.h>
#include <zephyr/cache.h>
#include <zephyr/sys/printk.h>

#define TEST_CYCLES        100
#define MAX_REPORTED_ERRS  5

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(psram0));
	volatile uint32_t *mem;
	uint64_t size64 = 0;
	uint64_t total_errs = 0;
	uint32_t words;
	uint32_t reported = 0;
	int64_t t_start;

	printk("PSRAM full-volume stress: %u cycles x 2 patterns\n", TEST_CYCLES);

	if (!device_is_ready(dev)) {
		printk("TEST FAIL: PSRAM device not ready\n");
		return 0;
	}
	if (memc_get_size(dev, &size64) || size64 == 0) {
		printk("TEST FAIL: cannot get PSRAM size\n");
		return 0;
	}
	mem = memc_get_mem_base(dev);
	if (mem == NULL) {
		printk("TEST FAIL: no memory-mapped base\n");
		return 0;
	}
	words = (uint32_t)(size64 / sizeof(uint32_t));
	printk("Device %s: %u KB mapped at %p\n",
	       dev->name, (uint32_t)(size64 / 1024U), (void *)mem);

	t_start = k_uptime_get();

	for (uint32_t cycle = 1; cycle <= TEST_CYCLES; cycle++) {
		int64_t t_cycle = k_uptime_get();
		uint32_t cycle_errs = 0;

		for (uint32_t pass = 0; pass < 2; pass++) {
			const uint32_t inv = pass ? 0xFFFFFFFFU : 0U;

			/* Fill the whole volume in one uninterrupted stream */
			for (uint32_t i = 0; i < words; i++) {
				mem[i] = (i * 4U) ^ inv;
			}

			/* Make sure the data hit the chip, not a CPU cache */
			sys_cache_data_flush_and_invd_all();

			/* Verify the whole volume on the device */
			for (uint32_t i = 0; i < words; i++) {
				uint32_t exp = (i * 4U) ^ inv;
				uint32_t got = mem[i];

				if (got != exp) {
					cycle_errs++;
					if (reported < MAX_REPORTED_ERRS) {
						printk("MISMATCH cycle %u pass %u "
						       "off 0x%08x: expected %08x "
						       "got %08x\n",
						       cycle, pass, i * 4U,
						       exp, got);
						reported++;
					}
				}
			}
		}

		total_errs += cycle_errs;
		printk("Cycle %3u/%u: %s (%lld ms)\n", cycle, TEST_CYCLES,
		       cycle_errs ? "ERRORS" : "ok",
		       k_uptime_get() - t_cycle);
	}

	printk("Total time: %lld s\n", (k_uptime_get() - t_start) / 1000);
	if (total_errs == 0) {
		printk("TEST PASS: %u cycles, %u KB verified twice per cycle, "
		       "0 errors\n", TEST_CYCLES, (uint32_t)(size64 / 1024U));
	} else {
		printk("TEST FAIL: %llu mismatched words over %u cycles\n",
		       total_errs, TEST_CYCLES);
	}
	return 0;
}
