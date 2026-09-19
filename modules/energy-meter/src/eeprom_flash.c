/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cedar,eeprom-flash: the flash API over an EEPROM device, so that ZMS - which
 * only speaks flash - can keep the energy counter in the SPI FRAM. Zephyr drives
 * FRAM through the EEPROM API and has no flash driver for it (reports/energy).
 *
 * FRAM needs no erase: the device reports no explicit erase, ZMS then never
 * erases, and flash_erase()/flash_flatten() fill with 0xFF. One page covers the
 * whole device. Reads and writes run in the caller's thread, which on this board
 * must have its stack in SRAM (SPI1 DMA).
 */

#define DT_DRV_COMPAT cedar_eeprom_flash

#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/flash.h>

#define EEPROM_NODE DT_INST_PHANDLE(0, eeprom)
#define EEPROM_SIZE DT_PROP(EEPROM_NODE, size)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "one cedar,eeprom-flash instance is supported");

static const struct device *const eeprom = DEVICE_DT_GET(EEPROM_NODE);

static const struct flash_parameters params = {
	.write_block_size = 1,
	.erase_value = 0xff,
	.caps = {.no_explicit_erase = true},
};

static int ef_read(const struct device *dev, off_t off, void *data, size_t len)
{
	ARG_UNUSED(dev);
	return eeprom_read(eeprom, off, data, len);
}

static int ef_write(const struct device *dev, off_t off, const void *data, size_t len)
{
	ARG_UNUSED(dev);
	return eeprom_write(eeprom, off, data, len);
}

static int ef_erase(const struct device *dev, off_t off, size_t len)
{
	return flash_fill(dev, params.erase_value, off, len);
}

static const struct flash_parameters *ef_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	return &params;
}

static int ef_get_size(const struct device *dev, uint64_t *size)
{
	ARG_UNUSED(dev);
	*size = EEPROM_SIZE;
	return 0;
}

static void ef_page_layout(const struct device *dev, const struct flash_pages_layout **layout,
			   size_t *layout_size)
{
	static const struct flash_pages_layout one_page = {
		.pages_count = 1,
		.pages_size = EEPROM_SIZE,
	};

	ARG_UNUSED(dev);
	*layout = &one_page;
	*layout_size = 1;
}

static DEVICE_API(flash, ef_api) = {
	.read = ef_read,
	.write = ef_write,
	.erase = ef_erase,
	.get_parameters = ef_get_parameters,
	.get_size = ef_get_size,
	.page_layout = ef_page_layout,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY,
		      &ef_api);
