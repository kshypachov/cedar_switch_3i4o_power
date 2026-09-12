//
// Created by Kirill Shypachov on 17.03.2026.
//

#include "wifi.h"
#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wifi, CONFIG_LOG_DEFAULT_LEVEL);

static const struct gpio_dt_spec wifi_reset_gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(wifi_reset), gpios);
static const struct gpio_dt_spec wifi_boot_gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(wifi_boot), gpios);

int wifi_reset_simple()
{
	int ret;

	if (!device_is_ready(wifi_reset_gpio.port)) {
		LOG_ERR("Error: GPIO device %s is not ready", wifi_reset_gpio.port->name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&wifi_reset_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure wifi_reset pin", ret);
		return ret;
	}

	ret = gpio_pin_set_raw(wifi_reset_gpio.port, wifi_reset_gpio.pin, 0);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to set wifi_reset low", ret);
		return ret;
	}

	k_msleep(100);

	ret = gpio_pin_set_raw(wifi_reset_gpio.port, wifi_reset_gpio.pin, 1);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to set wifi_reset high", ret);
		return ret;
	}

	k_msleep(100);
	return 0;
}


void wifi_init() {
    LOG_INF("Initializing WiFi reset pin...");
    if (!device_is_ready(wifi_reset_gpio.port)) {
        LOG_ERR("Error: GPIO device %s is not ready", wifi_reset_gpio.port->name);
        return;
    }

    LOG_INF("Initializing WiFi BOOT pin...");
    if (!device_is_ready(wifi_boot_gpio.port)) {
        LOG_ERR("Error: GPIO device %s is not ready", wifi_reset_gpio.port->name);
        return;
    }

    // int ret;
    // ret = gpio_pin_configure_dt(&wifi_reset_gpio, GPIO_OUTPUT_ACTIVE);
    // if (ret != 0) {
    //     LOG_ERR("Error %d: failed to configure wifi_reset pin", ret);
    //     return;
    // }
    //
    LOG_INF("WiFi reset pin configured and set to ACTIVE");

    /* В DTS платы GPIO_ACTIVE_LOW: 1 = линия в 0 (reset / BOOT-страп активны) */
    gpio_pin_set_dt(&wifi_reset_gpio, 1);
    gpio_pin_set_dt(&wifi_boot_gpio, 1);
    k_msleep(100);
    gpio_pin_set_dt(&wifi_reset_gpio, 0);
	k_msleep(200);
	gpio_pin_set_dt(&wifi_boot_gpio, 0);
    LOG_INF("WiFi reset pin set to INACTIVE");

}
