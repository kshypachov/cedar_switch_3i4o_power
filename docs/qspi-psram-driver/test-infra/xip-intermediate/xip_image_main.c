/*
 * XIP image: executes from the external QSPI NOR flash (through the ICACHE
 * remap window at 0x02000000) and uses the QPI PSRAM at 0x70000000 as data
 * memory. Both windows were armed by the first stage; this image contains no
 * OCTOSPI drivers at all, so nothing here disturbs the shared I/O manager.
 *
 * Scratch application, not part of the tree.
 */

#include <zephyr/kernel.h>

#define PSRAM_BASE ((volatile uint32_t *)0x70000000)
#define PSRAM_SIZE (4 * 1024 * 1024)

#define SWEEP_WORDS (256 * 1024) /* 1 MB */
#define PASSES      8

static inline uint32_t pattern(uint32_t idx)
{
	uint32_t x = idx * 2654435761u + 0x9e3779b9u;

	x ^= x >> 15;
	x *= 0x85ebca6bu;
	x ^= x >> 13;
	return x;
}

int main(void)
{
	volatile uint32_t *p = PSRAM_BASE;
	uint32_t bad = 0;

	printk("\n=== XIP from flash + PSRAM as data memory ===\n");
	printk("main() at %p, vector table at %p\n", (void *)&main, (void *)SCB->VTOR);

	if (((uintptr_t)&main & 0xff000000) != 0x02000000) {
		printk("RESULT: FAIL - not executing from the flash window\n");
		return 0;
	}
	printk("code runs from external flash, data goes to %p\n", (void *)PSRAM_BASE);

	for (int pass = 0; pass < PASSES; pass++) {
		uint32_t errors = 0;
		int64_t t0 = k_uptime_get();

		for (uint32_t i = 0; i < SWEEP_WORDS; i++) {
			p[i] = pattern(i + pass);
		}
		for (uint32_t i = 0; i < SWEEP_WORDS; i++) {
			if (p[i] != pattern(i + pass)) {
				errors++;
			}
		}
		bad += errors;
		printk("  pass %d: %4lld ms, psram errors %u\n",
		       pass, k_uptime_get() - t0, errors);
	}

	printk("RESULT: %s\n", bad ? "FAIL" : "PASS");
	return 0;
}
