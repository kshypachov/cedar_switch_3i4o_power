/*
 * First stage, runs from internal flash:
 *   1. programs the XIP image into the external QSPI NOR flash at offset 0,
 *   2. puts the flash controller into memory-mapped mode,
 *   3. jumps to the image, which then executes straight out of the flash
 *      window at 0x90000000 and uses the PSRAM on the muxed bus.
 *
 * Scratch application, not part of the tree.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <cmsis_core.h>
#include <string.h>

#include "xip_image.h"

#define NOR_NODE   DT_NODELABEL(ext_flash_ctrl)
#define OSPI1_NODE DT_NODELABEL(octospi1)

static const struct device *const nor = DEVICE_DT_GET(NOR_NODE);

#define NOR_MAP_BASE ((volatile const uint8_t *)DT_REG_ADDR_BY_IDX(OSPI1_NODE, 1))

/*
 * On STM32U5 the Cortex-M33 does not execute from the OCTOSPI window directly:
 * the ICACHE remaps a slice of the code space onto the external memory. This
 * is the same 0x02000000 window the board DTS uses for its code partition.
 */
#define XIP_WINDOW ((volatile const uint8_t *)0x02000000)

#define ERASE_BLOCK 4096

static int setup_icache_remap(void)
{
	/* Region programming requires the cache to be off */
	LL_ICACHE_Disable();
	while (LL_ICACHE_IsEnabled()) {
	}

	LL_ICACHE_DisableRegion(LL_ICACHE_REGION_0);
	LL_ICACHE_SetRegionBaseAddress(LL_ICACHE_REGION_0, (uint32_t)XIP_WINDOW);
	LL_ICACHE_SetRegionRemapAddress(LL_ICACHE_REGION_0, (uint32_t)NOR_MAP_BASE);
	LL_ICACHE_SetRegionSize(LL_ICACHE_REGION_0, LL_ICACHE_REGIONSIZE_4MB);
	LL_ICACHE_SetRegionMasterPort(LL_ICACHE_REGION_0, LL_ICACHE_MASTER2_PORT);
	LL_ICACHE_SetRegionOutputBurstType(LL_ICACHE_REGION_0, LL_ICACHE_OUTPUT_BURST_WRAP);
	LL_ICACHE_EnableRegion(LL_ICACHE_REGION_0);

	LL_ICACHE_Enable();
	return 0;
}

static uint8_t verify[1024];

static void psram_check(const char *when)
{
	volatile uint32_t *p = (volatile uint32_t *)0x70000000;
	uint32_t errors = 0;

	for (uint32_t i = 0; i < 4096; i++) {
		p[i] = i * 2654435761u;
	}
	sys_cache_data_flush_and_invd_all();
	for (uint32_t i = 0; i < 4096; i++) {
		if (p[i] != i * 2654435761u) {
			errors++;
		}
	}
	printk("psram check %-22s %u errors of 4096\n", when, errors);
}

static bool image_already_programmed(size_t len)
{
	for (size_t off = 0; off < len; off += sizeof(verify)) {
		size_t chunk = MIN(sizeof(verify), len - off);

		if (flash_read(nor, off, verify, chunk) != 0) {
			return false;
		}
		if (memcmp(verify, xip_image + off, chunk) != 0) {
			return false;
		}
	}
	return true;
}

static void jump_to_image(void)
{
	const uint32_t *vt = (const uint32_t *)XIP_WINDOW;
	uint32_t sp = vt[0];
	uint32_t pc = vt[1];

	printk("jumping to image: SP 0x%08x PC 0x%08x\n", sp, pc);
	k_msleep(50); /* let the console drain */

	__disable_irq();

	SysTick->CTRL = 0;
	SysTick->VAL = 0;

	for (int i = 0; i < 8; i++) {
		NVIC->ICER[i] = 0xffffffffu;
		NVIC->ICPR[i] = 0xffffffffu;
	}

	SCB->VTOR = (uint32_t)XIP_WINDOW;
	__DSB();
	__ISB();

	/* Hand over a clean MPU: our regions do not describe the next image */
	MPU->CTRL = 0;
	__DSB();
	__ISB();

	/*
	 * This image is built with HW stack protection, so MSPLIM/PSPLIM point
	 * into its own stacks. Clear them before handing the stack pointer to
	 * the next image, otherwise its first push takes a stack overflow fault.
	 */
	__set_MSPLIM(0);
	__set_PSPLIM(0);

	__set_CONTROL(0);
	__set_MSP(sp);
	__ISB();

	((void (*)(void))pc)();

	CODE_UNREACHABLE;
}

int main(void)
{
	size_t len = xip_image_len;
	size_t erase_len = ROUND_UP(len, ERASE_BLOCK);
	int ret;

	printk("\n=== stage1: program XIP image into QSPI flash ===\n");

	if (!device_is_ready(nor)) {
		printk("FAIL - flash not ready\n");
		return 0;
	}

	/*
	 * Program the image only when it is not already there, then reboot.
	 * At run time the flash must be read-only: erase/write traffic on
	 * OCTOSPI1 corrupts the PSRAM sharing the bus, so the boot that hands
	 * control to the image must not touch the flash for writing at all.
	 */
	if (image_already_programmed(len)) {
		printk("image already in flash, no write this boot\n");
		goto handover;
	}

	printk("image %u bytes, erasing %u bytes at offset 0\n",
	       (unsigned)len, (unsigned)erase_len);
	ret = flash_erase(nor, 0, erase_len);
	if (ret) {
		printk("FAIL - flash_erase: %d\n", ret);
		return 0;
	}

	ret = flash_write(nor, 0, xip_image, len);
	if (ret) {
		printk("FAIL - flash_write: %d\n", ret);
		return 0;
	}
	printk("written, verifying\n");

	for (size_t off = 0; off < len; off += sizeof(verify)) {
		size_t chunk = MIN(sizeof(verify), len - off);

		ret = flash_read(nor, off, verify, chunk);
		if (ret) {
			printk("FAIL - flash_read at 0x%x: %d\n", (unsigned)off, ret);
			return 0;
		}
		if (memcmp(verify, xip_image + off, chunk) != 0) {
			printk("FAIL - verify mismatch at 0x%x\n", (unsigned)off);
			return 0;
		}
	}
	printk("verify ok, rebooting so the handover boot never writes flash\n");
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);

handover:
	/* The read above leaves the controller memory-mapped; confirm through
	 * the window before handing control over.
	 */
	if (memcmp((const void *)NOR_MAP_BASE, xip_image, 256) != 0) {
		printk("FAIL - flash window does not show the image\n");
		return 0;
	}
	printk("flash window at %p shows the image\n", (void *)NOR_MAP_BASE);

	if (setup_icache_remap() != 0) {
		return 0;
	}
	if (memcmp((const void *)XIP_WINDOW, xip_image, 256) != 0) {
		printk("FAIL - remap window at %p does not show the image\n",
		       (void *)XIP_WINDOW);
		return 0;
	}
	printk("icache remap %p -> %p ok\n", (void *)XIP_WINDOW, (void *)NOR_MAP_BASE);

	/* Is the PSRAM still usable after all the flash traffic above? */
	{
		volatile uint32_t *p = (volatile uint32_t *)0x70000000;
		uint32_t errors = 0;

		for (uint32_t i = 0; i < 4096; i++) {
			p[i] = i * 2654435761u;
		}
		for (uint32_t i = 0; i < 4096; i++) {
			if (p[i] != i * 2654435761u) {
				errors++;
			}
		}
		printk("psram check in stage1: %u errors of 4096\n", errors);
	}

	jump_to_image();
	return 0;
}
