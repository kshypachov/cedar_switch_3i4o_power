/*
 * Streaming stress test for the QPI PSRAM, run at the highest rate the CPU
 * can push. Two phases per pass, both without any pacing:
 *
 *   A - tight 32-bit store/load loop over the whole device, the fastest
 *       access pattern available and the one that starves the chip refresh
 *       first if the controller never releases nCS;
 *   B - memcpy() of a precomputed buffer into every chunk back to back, the
 *       pattern a driver streaming a file into PSRAM produces.
 *
 * Each pass uses a fresh pseudo-random seed. Any corruption is reported with
 * the offset where it starts.
 *
 * Scratch application, not part of the tree.
 */

#include <zephyr/kernel.h>
#include <string.h>

#ifndef PSRAM_ADDR
#define PSRAM_ADDR 0x70000000
#endif

#ifndef PSRAM_TOTAL
#define PSRAM_TOTAL (8 * 1024 * 1024)
#endif

#define CHUNK  4096
#define PASSES 100

static uint8_t buf[CHUNK];

static inline uint32_t word_at(uint32_t seed, uint32_t i)
{
	uint32_t x = seed + i * 2654435761u;

	x ^= x >> 15;
	x *= 0x85ebca6bu;
	x ^= x >> 13;
	x *= 0xc2b2ae35u;
	x ^= x >> 16;
	return x;
}

/* Phase A: tightest possible streaming, nothing between the stores */
static uint32_t phase_tight(uint32_t seed, uint32_t *first_bad, int64_t *ms)
{
	volatile uint32_t *p = (volatile uint32_t *)PSRAM_ADDR;
	const uint32_t words = PSRAM_TOTAL / 4;
	uint32_t errors = 0;
	int64_t t0 = k_uptime_get();

	for (uint32_t i = 0; i < words; i++) {
		p[i] = seed + i;
	}
	for (uint32_t i = 0; i < words; i++) {
		if (p[i] != seed + i) {
			if (!errors) {
				*first_bad = i * 4;
			}
			errors++;
		}
	}

	*ms = k_uptime_get() - t0;
	return errors;
}

/* Phase B: file-style streaming, buffer prepared once so copies run back to back */
static uint32_t phase_stream(uint32_t seed, uint32_t *first_bad, int64_t *ms)
{
	uint8_t *psram = (uint8_t *)PSRAM_ADDR;
	const uint32_t chunks = PSRAM_TOTAL / CHUNK;
	uint32_t *w = (uint32_t *)buf;
	uint32_t errors = 0;
	int64_t t0;

	for (uint32_t j = 0; j < CHUNK / 4; j++) {
		w[j] = word_at(seed, j);
	}

	t0 = k_uptime_get();
	for (uint32_t c = 0; c < chunks; c++) {
		memcpy(psram + c * CHUNK, buf, CHUNK);
	}
	for (uint32_t c = 0; c < chunks; c++) {
		if (memcmp(psram + c * CHUNK, buf, CHUNK) != 0) {
			const uint8_t *got = psram + c * CHUNK;

			for (uint32_t b = 0; b < CHUNK; b++) {
				if (got[b] != buf[b]) {
					if (!errors) {
						*first_bad = c * CHUNK + b;
					}
					errors++;
				}
			}
		}
	}
	*ms = k_uptime_get() - t0;
	return errors;
}

int main(void)
{
	uint32_t seed = 0x12345678u;
	uint32_t failed_passes = 0;
	uint64_t total_errors = 0;

	printk("\n=== psram streaming stress (%s) ===\n", MODEL_NAME);
	printk("code at %p, %u MB at 0x%08x, %d passes, no pacing\n",
	       (void *)&main, PSRAM_TOTAL / (1024 * 1024), PSRAM_ADDR, PASSES);

	for (int pass = 0; pass < PASSES; pass++) {
		uint32_t bad_a = 0, bad_b = 0;
		uint32_t first_a = 0, first_b = 0;
		int64_t ms_a = 0, ms_b = 0;

		seed ^= k_cycle_get_32();
		seed = word_at(seed, pass);

		bad_a = phase_tight(seed, &first_a, &ms_a);
		bad_b = phase_stream(seed, &first_b, &ms_b);

		total_errors += (uint64_t)bad_a + bad_b;
		if (bad_a || bad_b) {
			failed_passes++;
			printk("pass %3d seed %08x  tight %4lld ms BAD %u @0x%x | stream %4lld ms BAD %u @0x%x\n",
			       pass, seed, ms_a, bad_a, first_a, ms_b, bad_b, first_b);
		} else if ((pass % 10) == 0 || pass == PASSES - 1) {
			printk("pass %3d seed %08x  tight %4lld ms | stream %4lld ms  clean\n",
			       pass, seed, ms_a, ms_b);
		}
	}

	printk("SUMMARY: %d passes, %u failed, %llu bad bytes/words total\n",
	       PASSES, failed_passes, total_errors);
	printk("RESULT: %s\n", failed_passes ? "FAIL" : "PASS");
	return 0;
}
