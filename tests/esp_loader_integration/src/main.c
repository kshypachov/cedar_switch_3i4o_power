/*
 * P0 integration check for esp-serial-flasher on the Cedar switch board.
 *
 * Non-destructive on purpose: it enters the ROM download mode, identifies the
 * coprocessor and returns it to normal boot. Nothing is erased or written, so
 * this can be run on a board carrying production coprocessor firmware.
 *
 * What a pass proves: the west module builds, the devicetree node is wired to
 * the right UART and straps, EN/BOOT timing gets the C6 into the ROM loader,
 * and the serial protocol completes a round trip.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <esp_loader.h>
#include <esp_loader_io.h>
#include <zephyr_port.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(esp_loader_check, LOG_LEVEL_INF);

static const struct device *const loader_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_esp_loader));

static const char *target_name(target_chip_t chip)
{
	switch (chip) {
	case ESP8266_CHIP:  return "ESP8266";
	case ESP32_CHIP:    return "ESP32";
	case ESP32S2_CHIP:  return "ESP32-S2";
	case ESP32C3_CHIP:  return "ESP32-C3";
	case ESP32S3_CHIP:  return "ESP32-S3";
	case ESP32C2_CHIP:  return "ESP32-C2";
	case ESP32C5_CHIP:  return "ESP32-C5";
	case ESP32H2_CHIP:  return "ESP32-H2";
	case ESP32C6_CHIP:  return "ESP32-C6";
	case ESP32P4_CHIP:  return "ESP32-P4";
	case ESP32C61_CHIP: return "ESP32-C61";
	default:            return "unknown";
	}
}

int main(void)
{
	esp_loader_connect_args_t args;
	esp_loader_error_t err;
	target_chip_t chip;

	LOG_INF("esp-serial-flasher integration check");

	if (!device_is_ready(loader_dev)) {
		LOG_ERR("esp-loader device not ready");
		return -ENODEV;
	}

	esp_loader_t *loader = esp_loader_from_device(loader_dev);
	const esp_loader_connect_args_t *dt_args =
		esp_loader_connect_args_from_device(loader_dev);

	/* esp_loader_connect() takes a mutable pointer; keep the DT values intact. */
	args = *dt_args;

	LOG_INF("connecting: %d trials, %u ms sync timeout",
		args.trials, args.sync_timeout);

	err = esp_loader_connect(loader, &args);
	if (err != ESP_LOADER_SUCCESS) {
		LOG_ERR("connect failed: %d", err);
		LOG_ERR("check USART3 wiring, EN/BOOT straps, and that nothing "
			"else holds the UART");
		return -EIO;
	}

	chip = esp_loader_get_target(loader);
	LOG_INF("connected, target reports: %s", target_name(chip));

	/* Always hand the coprocessor back, including on a wrong-chip result. */
	esp_loader_reset_target(loader);
	LOG_INF("target released to normal boot");

	if (chip != ESP32C6_CHIP) {
		LOG_ERR("expected ESP32-C6, got %s", target_name(chip));
		return -ENOTSUP;
	}

	LOG_INF("PASS");
	return 0;
}
