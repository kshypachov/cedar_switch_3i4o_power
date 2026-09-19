/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The status LED on PC2 (board overlay, status_led): blinks, 500 ms on and 500 ms
 * off, while the firmware runs (owner, 2026-09-19).
 *
 * Its own thread at preemptive priority 5: above the application's workers
 * (priority 10, which do not time-slice, e.g. the image hash during an update),
 * so a long job does not stop it; a cooperative thread that holds the CPU, a
 * fault or a hang does, and the LED then stays in its last state.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(status_led, LOG_LEVEL_INF);

#define STATUS_LED_HALF_PERIOD_MS 500

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(status_led), gpios);

static void status_led_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!gpio_is_ready_dt(&led) || gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_ERR("status LED (PC2) not available");
		return;
	}
	for (;;) {
		(void)gpio_pin_toggle_dt(&led);
		k_msleep(STATUS_LED_HALF_PERIOD_MS);
	}
}

K_THREAD_DEFINE(status_led, 1024, status_led_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0,
		0);
