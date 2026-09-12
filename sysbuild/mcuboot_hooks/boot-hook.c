//
// Created by Kirill Shypachov on 24.04.2026.
// Moved from cedar_switch_3i4o_power_bootloader/boot-hook.c into the sysbuild
// MCUboot image of the application.
//
#include <stdbool.h>
#include <stdint.h>

#include <stm32u5xx.h>
#include <stm32_ll_icache.h>
#include "stm32u5xx_hal.h"

#include <bootutil/boot_hooks.h>
#include <bootutil/boot_public_hooks.h>
#include <bootutil/bootutil_public.h>

#include "bootutil/bootutil_log.h"
#include "zephyr/cache.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/bbram.h>

BOOT_LOG_MODULE_REGISTER(boot_hook);

#define FWUPD_CONFIRM_RTC_PATTERN 0xC0DEF00DU

/*
 * ICACHE remap for STM32U585:
 * alias region starting at 0x9000_0000 to 0x0200_0000.
 */
static void boot_configure_alias_9000_to_0200(void)
{
#if defined(ICACHE) && defined(ICACHE_CRRx_REN) && defined(ICACHE_CRRx_RSIZE_Pos)
	static bool configured;

	if (configured) {
		return;
	}

	/* Required sequence:
	 * ICACHE disable -> program CRR0 -> invalidate/DSB/ISB -> ICACHE enable.
	 */
	LL_ICACHE_RegionTypeDef pICACHE_RegionStruct = {0};

	LL_ICACHE_Disable();

	pICACHE_RegionStruct.BaseAddress = 0x02000000;
	pICACHE_RegionStruct.RemapAddress = 0x90000000;
	pICACHE_RegionStruct.Size = LL_ICACHE_REGIONSIZE_4MB;
	pICACHE_RegionStruct.TrafficRoute = LL_ICACHE_MASTER2_PORT;
	pICACHE_RegionStruct.OutputBurstType = LL_ICACHE_OUTPUT_BURST_WRAP;
	LL_ICACHE_ConfigRegion(LL_ICACHE_REGION_0, &pICACHE_RegionStruct);


	LL_ICACHE_Enable();
	while (!LL_ICACHE_IsEnabled()) {
	}

	configured = true;
	BOOT_LOG_INF("ICACHE alias: 0x02000000 -> 0x90000000 (4MB)");
    {
        volatile const uint8_t *p = (volatile const uint8_t *)0x90000000U;

        BOOT_LOG_INF("0x90000000..0x9000001F:");
        BOOT_LOG_INF(
            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
            p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
        BOOT_LOG_INF(
            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23],
            p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31]);
    }


    {
        volatile const uint8_t *d = (volatile const uint8_t *)0x02000000U;

        BOOT_LOG_INF("Alias[0x02000000..0x0200001F]:");
        BOOT_LOG_INF(
            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
            d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
        BOOT_LOG_INF(
            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            d[16], d[17], d[18], d[19], d[20], d[21], d[22], d[23],
            d[24], d[25], d[26], d[27], d[28], d[29], d[30], d[31]);
    }
#else
    BOOT_LOG_WRN("ICACHE remap is not available on this build");
#endif
}

/*
 * PSRAM is memory-mapped at 0x70000000 only when its driver runs in the
 * bootloader. This MCUboot image does not enable MEMC (the application brings
 * PSRAM up with the qspi-psram driver), so touching 0x70000000 here would fault.
 */
#if defined(CONFIG_MEMC)
static void boot_check_psram(void)
{
	volatile uint8_t *p8 = (volatile uint8_t *)0x70000000U;

	BOOT_LOG_INF("PSRAM 0x70000000..0x7000001F:");
	BOOT_LOG_INF(
		"%02x %02x %02x %02x %02x %02x %02x %02x "
		"%02x %02x %02x %02x %02x %02x %02x %02x",
		p8[0],  p8[1],  p8[2],  p8[3],
		p8[4],  p8[5],  p8[6],  p8[7],
		p8[8],  p8[9],  p8[10], p8[11],
		p8[12], p8[13], p8[14], p8[15]);
	BOOT_LOG_INF(
		"%02x %02x %02x %02x %02x %02x %02x %02x "
		"%02x %02x %02x %02x %02x %02x %02x %02x",
		p8[16], p8[17], p8[18], p8[19],
		p8[20], p8[21], p8[22], p8[23],
		p8[24], p8[25], p8[26], p8[27],
		p8[28], p8[29], p8[30], p8[31]);

	/* Write/read test at offset 0x1000 to avoid stomping on address 0 */
	static const uint32_t wr[8] = {
		0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x9ABCDEF0,
		0x55AA55AA, 0xAA55AA55, 0x0F0F0F0F, 0xF0F0F0F0,
	};
	volatile uint32_t *t = (volatile uint32_t *)(0x70000000U + 0x1000U);

	for (int i = 0; i < 8; i++) {
		t[i] = wr[i];
	}
	__DSB();

	/* Invalidate D-cache for the test region so the readback comes
	 * from actual PSRAM, not a dirty cache line.
	 */
	sys_cache_data_flush_and_invd_range((void *)t, 32);

	bool ok = true;

	for (int i = 0; i < 8; i++) {
		if (t[i] != wr[i]) {
			BOOT_LOG_ERR("PSRAM[+0x%04x] wrote=0x%08x read=0x%08x",
				     0x1000 + i * 4, wr[i], t[i]);
			ok = false;
		}
	}

	if (ok) {
		BOOT_LOG_INF("PSRAM write/read: PASS");
	} else {
		BOOT_LOG_ERR("PSRAM write/read: FAIL");
	}
}
#endif /* CONFIG_MEMC */


static void boot_confirm_from_bbram_pattern(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(bbram)) && DT_NODE_HAS_STATUS(DT_NODELABEL(bbram), okay)
	const struct device *bbram = DEVICE_DT_GET(DT_NODELABEL(bbram));
	size_t bbram_size = 0;
	uint32_t pattern = 0;
	uint32_t cleared = 0;
	int rc;

	if (bbram == NULL) {
		BOOT_LOG_WRN("BBRAM node exists but device pointer is NULL");
		return;
	}

	if (!device_is_ready(bbram)) {
		BOOT_LOG_WRN("BBRAM device '%s' is not ready", bbram->name);
		return;
	}

	rc = bbram_get_size(bbram, &bbram_size);
	if (rc != 0 || bbram_size < sizeof(pattern)) {
		BOOT_LOG_WRN("BBRAM size/read check failed: rc=%d size=%u",
			     rc, (unsigned)bbram_size);
		return;
	}

	rc = bbram_read(bbram, 0, sizeof(pattern), (uint8_t *)&pattern);
	if (rc != 0) {
		BOOT_LOG_WRN("bbram_read failed: %d", rc);
		return;
	}

	rc = bbram_write(bbram, 0, sizeof(cleared), (const uint8_t *)&cleared);
	if (rc != 0) {
		BOOT_LOG_WRN("Failed to clear BBRAM pattern after read: %d", rc);
	}

	if (pattern != FWUPD_CONFIRM_RTC_PATTERN) {
		return;
	}

	rc = boot_set_confirmed();
	if (rc != 0) {
		BOOT_LOG_ERR("boot_set_confirmed failed: %d", rc);
		return;
	}

	BOOT_LOG_INF("Image confirmed via BBRAM pattern");
#else
	BOOT_LOG_DBG("No /soc/.../backup_regs (label: bbram), skipping confirm check");
#endif
}

fih_ret boot_go_hook(struct boot_rsp *rsp)
{
    (void)rsp;

    boot_confirm_from_bbram_pattern();
    boot_configure_alias_9000_to_0200();
#if defined(CONFIG_MEMC)
    boot_check_psram();
#endif
    return FIH_BOOT_HOOK_REGULAR;
}
