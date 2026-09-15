/*
 * P6 first measurement: what is on the ESP32-C6's flash, read through the ROM loader.
 *
 * Read-only on purpose. Board B's C6 must receive its first image through the web
 * updater (owner, 2026-09-13), so this image never calls a write, erase or begin
 * command: it enters download mode, identifies the chip, reads the headers the
 * updater's decisions depend on and hands the chip back to normal boot.
 *
 * The ROM READ_FLASH command returns 64 bytes at a time, so the regions are kept to
 * what the design needs: the second-stage bootloader's header and description, the
 * partition table, both otadata copies, and the head of each app slot.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/crc.h>

#include <esp_loader.h>
#include <esp_loader_io.h>
#include <zephyr_port.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(esp_flash_probe, LOG_LEVEL_INF);

static const struct device *const loader_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_esp_loader));

struct region {
	uint32_t address;
	uint32_t length;
	const char *name;
};

static const struct region regions[] = {
	{0x000000, 4096, "bootloader (0x0)"},
	{0x008000, 3072, "partition table (0x8000)"},
	{0x009000, 256, "nvs head (0x9000)"},
	{0x00d000, 64, "otadata[0] (0xd000)"},
	{0x00e000, 64, "otadata[1] (0xe000)"},
	{0x00f000, 64, "phy_init head (0xf000)"},
	{0x010000, 512, "ota_0 head (0x10000)"},
	{0x1d0000, 512, "ota_1 head (0x1d0000)"},
};

static uint8_t buf[4096];

/* Rows that are not all 0xFF, 16 bytes each; runs of blank rows are counted. */
static void dump(uint32_t base, const uint8_t *data, uint32_t len)
{
	uint32_t blank_run = 0;

	for (uint32_t off = 0; off < len; off += 16) {
		uint32_t n = MIN(16U, len - off);
		bool blank = true;
		char line[16 * 3 + 1];
		size_t pos = 0;

		for (uint32_t i = 0; i < n; i++) {
			blank = blank && data[off + i] == 0xFF;
			pos += snprintf(&line[pos], sizeof(line) - pos, "%02x ", data[off + i]);
		}
		if (blank) {
			blank_run++;
			continue;
		}
		if (blank_run > 0) {
			printk("    ... %u blank rows\n", blank_run);
			blank_run = 0;
		}
		printk("    %06x: %s\n", base + off, line);
	}
	if (blank_run > 0) {
		printk("    ... %u blank rows\n", blank_run);
	}
}

int main(void)
{
	esp_loader_connect_args_t args;
	esp_loader_error_t err;
	uint32_t flash_size = 0;
	uint8_t mac[6];
	int failures = 0;

	LOG_INF("esp32-c6 flash probe (read-only)");

	if (!device_is_ready(loader_dev)) {
		LOG_ERR("esp-loader device not ready");
		return -ENODEV;
	}

	esp_loader_t *loader = esp_loader_from_device(loader_dev);

	args = *esp_loader_connect_args_from_device(loader_dev);
	int64_t t = k_uptime_get();

	err = esp_loader_connect(loader, &args);
	if (err != ESP_LOADER_SUCCESS) {
		LOG_ERR("connect failed: %d", err);
		return -EIO;
	}
	LOG_INF("connected in %lld ms, target %d (ESP32-C6 is %d)", k_uptime_get() - t,
		esp_loader_get_target(loader), ESP32C6_CHIP);

	err = esp_loader_read_mac(loader, mac);
	LOG_INF("read_mac: %d, %02x:%02x:%02x:%02x:%02x:%02x", err, mac[0], mac[1], mac[2],
		mac[3], mac[4], mac[5]);

	err = esp_loader_flash_detect_size(loader, &flash_size);
	LOG_INF("flash_detect_size: %d, %u bytes", err, flash_size);

	for (size_t i = 0; i < ARRAY_SIZE(regions); i++) {
		const struct region *r = &regions[i];
		uint32_t blank = 0;

		t = k_uptime_get();
		err = esp_loader_flash_read(loader, buf, r->address, r->length);
		int64_t ms = k_uptime_get() - t;

		if (err != ESP_LOADER_SUCCESS) {
			LOG_ERR("read %s: error %d after %lld ms", r->name, err, ms);
			failures++;
			continue;
		}
		for (uint32_t b = 0; b < r->length; b++) {
			blank += buf[b] == 0xFF;
		}
		printk("REGION %s addr=0x%06x len=%u ms=%lld blank=%u crc32=%08x\n", r->name,
		       r->address, r->length, ms, blank, crc32_ieee(buf, r->length));
		dump(r->address, buf, r->length);
	}

	/* Always hand the coprocessor back, whatever the reads did. */
	esp_loader_reset_target(loader);
	LOG_INF("target released to normal boot");

	if (failures) {
		LOG_ERR("FAIL: %d region(s) unread", failures);
		return -EIO;
	}
	LOG_INF("PASS");
	return 0;
}
