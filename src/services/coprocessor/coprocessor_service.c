/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See coprocessor_service.h and README.md.
 *
 * Three handlers can own USART3's interrupt, and coprocessor-manager decides
 * which one is installed:
 *
 * - console: the ISR copies received bytes into console_rx and nothing else;
 *   the worker feeds them to the ESP32 line assembler and the lines to
 *   log-store.
 * - usb_bridge: the ISR passes bytes to the board's CDC (cdc_acm_uart0) and
 *   sends what the host wrote. A host that does not read its end costs bytes,
 *   counted, never a stopped receiver: to_host full means drop.
 * - flashing: no handler. In P5 a stand-in: nothing reads the UART.
 *
 * Everything touched from an interrupt - the rings, the counters, this file's
 * data - stays in SRAM: this is a library of its own, not part of `app`, which
 * is relocated into PSRAM.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <esp32_log_source/esp32_log_source.h>
#include <log_store/log_store.h>

#include "services/network/net_adapter.h"
#include "coprocessor_service.h"

LOG_MODULE_REGISTER(coprocessor_service, LOG_LEVEL_INF);

#define WORKER_STACK_SIZE 2048
/* Below the network worker (10), above the log thread (13): lines should be in
 * the ring before the log thread formats the STM32's messages about them. */
#define WORKER_PRIORITY   K_PRIO_PREEMPT(12)
#define POLL_MS           20
#define DTR_POLL_MS       100
/* DTR low this long after a bridge it started hands the UART back. */
#define DTR_RELEASE_MS    1000

/* Pulse lengths of src/plugin_wifi/wifi.c, the sequence proven on this board
 * in P0 (tests/esp_loader_integration). */
#define EN_LOW_MS         100
#define BOOT_HOLD_MS      200

/* 8 KiB is 700 ms of a C6 printing at 115200. Measured on board B (image 1,
 * reports/p5/hw): the worker waits up to ~600 ms while the W5500's cooperative
 * thread reopens its socket three times in a row; 2 KiB (178 ms) overflowed a
 * dozen times in two minutes. */
#define CONSOLE_RING_SIZE 8192
#define BRIDGE_RING_SIZE  1024

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(usart3));
static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
static const struct gpio_dt_spec en = GPIO_DT_SPEC_GET(DT_NODELABEL(wifi_reset), gpios);
static const struct gpio_dt_spec boot = GPIO_DT_SPEC_GET(DT_NODELABEL(wifi_boot), gpios);

RING_BUF_DECLARE(console_rx, CONSOLE_RING_SIZE);
RING_BUF_DECLARE(to_host, BRIDGE_RING_SIZE);
RING_BUF_DECLARE(to_chip, BRIDGE_RING_SIZE);

static atomic_t rx_activity;
static atomic_t rx_bytes;
/* Bytes the console ring had no room for, pending for the worker, and in total. */
static atomic_t console_overflows;
static atomic_t console_bytes_lost;
static atomic_t console_overflow_episodes;
static atomic_t dropped_to_host;
static atomic_t dropped_to_chip;
static atomic_t host_bytes_discarded;
/*
 * 1 while the peer's transmit interrupt is off because its ring was empty.
 * Enabling a CDC's TX queues a work item on the USB thread, so the receiving side
 * enables it only when it takes this flag (atomic_cas) - once per empty-to-data
 * transition, not once per byte (11.5 kHz from USART3, measured in reports/p5).
 */
static atomic_t host_tx_idle = ATOMIC_INIT(1);
static atomic_t chip_tx_idle = ATOMIC_INIT(1);
static atomic_t host_tx_wakeups;
/* enum coprocessor_uart_mode of the installed handler; UNAVAILABLE for none. */
static atomic_t attached = ATOMIC_INIT(COPROCESSOR_UART_UNAVAILABLE);
static atomic_t dtr_follow = ATOMIC_INIT(1);

static struct uart_config console_config;
static bool console_config_valid;

static K_SEM_DEFINE(wake, 0, 1);
static K_MUTEX_DEFINE(asm_lock);
static struct esp32_log_assembler assembler;
static uint32_t console_generation = 1;
static bool banner_seen;
static bool started;

K_THREAD_STACK_DEFINE(coprocessor_worker_stack, WORKER_STACK_SIZE);
static struct k_thread worker_thread;

/* log-store is initialised by zephyr-log-source (CONFIG_ZEPHYR_LOG_SOURCE_INIT_STORE)
 * before any thread runs; this service only appends to it. */

/* -- interrupt handlers --------------------------------------------------------- */

static void console_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	atomic_inc(&rx_activity);
	/* void in this tree: it only latches the pending flags. */
	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev) > 0) {
		uint8_t buf[16];
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}
		atomic_add(&rx_bytes, n);
		const uint32_t put = ring_buf_put(&console_rx, buf, (uint32_t)n);

		if (put < (uint32_t)n) {
			atomic_add(&console_overflows, n - (int)put);
		}
	}
	if (ring_buf_size_get(&console_rx) > CONSOLE_RING_SIZE / 2U) {
		k_sem_give(&wake);
	}
}

/*
 * Send what @p ring holds on @p dev. When it is empty: TX off, then mark @p idle,
 * then look again - a byte the other side queued between the first look and the
 * mark would otherwise wait with TX off, since that side saw the flag still clear.
 */
static void fill_from(const struct device *dev, struct ring_buf *ring, atomic_t *idle)
{
	uint8_t *data;
	uint32_t len = ring_buf_get_claim(ring, &data, 64);

	if (len == 0U) {
		(void)ring_buf_get_finish(ring, 0U);
		uart_irq_tx_disable(dev);
		atomic_set(idle, 1);
		if (!ring_buf_is_empty(ring) && atomic_cas(idle, 1, 0)) {
			uart_irq_tx_enable(dev);
		}
		return;
	}
	int sent = uart_fifo_fill(dev, data, (int)len);

	(void)ring_buf_get_finish(ring, sent > 0 ? (uint32_t)sent : 0U);
}

/* The other side of fill_from(): wake @p dev's TX only on the transition. */
static void wake_tx(const struct device *dev, atomic_t *idle)
{
	if (atomic_cas(idle, 1, 0)) {
		uart_irq_tx_enable(dev);
	}
}

static void bridge_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	atomic_inc(&rx_activity);
	/* void in this tree: it only latches the pending flags. */
	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev) > 0) {
		uint8_t buf[16];
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}
		atomic_add(&rx_bytes, n);
		uint32_t put = ring_buf_put(&to_host, buf, (uint32_t)n);

		if (put < (uint32_t)n) {
			atomic_add(&dropped_to_host, n - (int)put);
		}
		if (atomic_get(&host_tx_idle) != 0) {
			atomic_inc(&host_tx_wakeups);
		}
		wake_tx(cdc, &host_tx_idle);
	}
	if (uart_irq_tx_ready(dev) > 0) {
		fill_from(dev, &to_chip, &chip_tx_idle);
	}
}

/* Installed for good at start. Outside the bridge, what the host sends is read
 * and dropped, so its end never backs up. */
static void cdc_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	/* void in this tree: it only latches the pending flags. */
	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev) > 0) {
		uint8_t buf[64];
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}
		if (atomic_get(&attached) == COPROCESSOR_UART_USB_BRIDGE) {
			uint32_t put = ring_buf_put(&to_chip, buf, (uint32_t)n);

			if (put < (uint32_t)n) {
				atomic_add(&dropped_to_chip, n - (int)put);
			}
			wake_tx(uart, &chip_tx_idle);
		} else {
			atomic_add(&host_bytes_discarded, n);
		}
	}
	if (uart_irq_tx_ready(dev) > 0) {
		fill_from(dev, &to_host, &host_tx_idle);
	}
}

/* -- the ESP32 log source -------------------------------------------------------- */

static void emit_line(void *ctx, const struct esp32_log_line *line)
{
	ARG_UNUSED(ctx);

	const struct log_store_entry e = {
		.source = LOG_STORE_ESP32,
		.level = line->level,
		.kind = LOG_STORE_KIND_MESSAGE,
		.truncated = line->truncated,
		.generation = console_generation,
		.uptime_ms = (uint64_t)k_uptime_get(),
		.module = line->module,
		.module_len = line->module_len,
		.text = line->text,
		.text_len = line->text_len,
	};

	if (log_store_append(&e) != 0) {
		log_store_count_loss(LOG_STORE_ESP32, LOG_STORE_LOSS_BACKEND, 1);
	}
	if (line->rom_banner) {
		/* Told to the manager after asm_lock is released: the manager calls
		 * marker() with its own mutex held, which takes asm_lock. */
		banner_seen = true;
	}
}

/* asm_lock held. */
static void drain_console(int64_t now)
{
	uint8_t buf[128];
	uint32_t n;
	const atomic_val_t lost = atomic_set(&console_overflows, 0);

	while ((n = ring_buf_get(&console_rx, buf, sizeof(buf))) > 0U) {
		esp32_log_feed(&assembler, buf, n, now);
	}
	if (lost > 0) {
		/* One damaged line per episode is what the store counts: how many
		 * lines the lost bytes held cannot be known. The bytes are kept for
		 * `coproc status`. */
		esp32_log_note_overflow(&assembler);
		log_store_count_loss(LOG_STORE_ESP32, LOG_STORE_LOSS_UART, 1);
		atomic_add(&console_bytes_lost, lost);
		atomic_inc(&console_overflow_episodes);
	}
}

/* -- the platform ------------------------------------------------------------------ */

static void restore_console_config(void)
{
	struct uart_config now;

	if (console_config_valid && uart_config_get(uart, &now) == 0 &&
	    now.baudrate != console_config.baudrate) {
		(void)uart_configure(uart, &console_config);
	}
}

static int op_attach(void *ctx, enum coprocessor_uart_mode owner)
{
	ARG_UNUSED(ctx);

	switch (owner) {
	case COPROCESSOR_UART_CONSOLE:
		restore_console_config();
		ring_buf_reset(&console_rx);
		uart_irq_callback_user_data_set(uart, console_isr, NULL);
		atomic_set(&attached, owner);
		uart_irq_rx_enable(uart);
		return 0;
	case COPROCESSOR_UART_USB_BRIDGE:
		ring_buf_reset(&to_host);
		ring_buf_reset(&to_chip);
		/* Both directions start idle with TX off (detach turned USART3's off;
		 * the CDC's turns itself off on its first empty look). */
		atomic_set(&host_tx_idle, 1);
		atomic_set(&chip_tx_idle, 1);
		uart_irq_callback_user_data_set(uart, bridge_uart_isr, NULL);
		atomic_set(&attached, owner);
		uart_irq_rx_enable(uart);
		return 0;
	case COPROCESSOR_UART_FLASHING:
		/* P5: the stand-in flasher reads nothing. P6: esp-serial-flasher's
		 * tty_serial opens the UART itself after this returns. */
		atomic_set(&attached, owner);
		return 0;
	default:
		return -EINVAL;
	}
}

static int op_detach(void *ctx)
{
	ARG_UNUSED(ctx);

	uart_irq_rx_disable(uart);
	uart_irq_tx_disable(uart);
	uart_irq_callback_user_data_set(uart, NULL, NULL);
	atomic_set(&attached, COPROCESSOR_UART_UNAVAILABLE);

	return 0;
}

static uint32_t op_rx_activity(void *ctx)
{
	ARG_UNUSED(ctx);

	return (uint32_t)atomic_get(&rx_activity);
}

static int op_reset(void *ctx, bool download)
{
	int rc;

	ARG_UNUSED(ctx);

	/* gpio-leds left both lines configured and inactive; the ESP-Hosted
	 * driver's reset line (the same PA8) is not touched after its init. */
	rc = gpio_pin_configure_dt(&en, GPIO_OUTPUT_INACTIVE);
	rc = rc ? rc : gpio_pin_configure_dt(&boot, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return rc;
	}
	if (download) {
		(void)gpio_pin_set_dt(&boot, 1);
	}
	(void)gpio_pin_set_dt(&en, 1);
	k_msleep(EN_LOW_MS);
	(void)gpio_pin_set_dt(&en, 0);
	if (download) {
		k_msleep(BOOT_HOLD_MS);
		(void)gpio_pin_set_dt(&boot, 0);
	}

	return 0;
}

static bool op_transport_ready(void *ctx)
{
	ARG_UNUSED(ctx);

	return net_adapter_wifi_present();
}

static void op_marker(void *ctx, enum log_store_kind kind, uint32_t generation, const char *text)
{
	ARG_UNUSED(ctx);

	k_mutex_lock(&asm_lock, K_FOREVER);
	/* Whatever the console received before it stopped belongs before the marker. */
	drain_console(k_uptime_get());
	esp32_log_flush(&assembler);
	if (kind == LOG_STORE_KIND_RESET) {
		esp32_log_reset(&assembler);
		console_generation = generation;
	}

	const struct log_store_entry e = {
		.source = LOG_STORE_ESP32,
		.level = LOG_STORE_LEVEL_NONE,
		.kind = kind,
		.generation = generation,
		.uptime_ms = (uint64_t)k_uptime_get(),
		.module = "coprocessor",
		.module_len = sizeof("coprocessor") - 1U,
		.text = text,
		.text_len = strlen(text),
	};

	if (log_store_append(&e) != 0) {
		log_store_count_loss(LOG_STORE_ESP32, LOG_STORE_LOSS_BACKEND, 1);
	}
	k_mutex_unlock(&asm_lock);
}

static int64_t op_now(void *ctx)
{
	ARG_UNUSED(ctx);

	return k_uptime_get();
}

static void op_sleep(void *ctx, uint32_t ms)
{
	ARG_UNUSED(ctx);

	k_msleep(ms);
}

static const struct coprocessor_platform platform = {
	.uart_attach = op_attach,
	.uart_detach = op_detach,
	.uart_rx_activity = op_rx_activity,
	.c6_reset = op_reset,
	.transport_ready = op_transport_ready,
	.marker = op_marker,
	.now_ms = op_now,
	.sleep_ms = op_sleep,
};

/* -- the worker ----------------------------------------------------------------------- */

static bool dtr_high;
static bool dtr_bridge;
static int64_t dtr_low_since;
static uint32_t bridge_baud;

static void follow_dtr(int64_t now)
{
	uint32_t dtr = 0;
	struct coprocessor_status st;

	if (atomic_get(&dtr_follow) == 0 ||
	    uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr) != 0) {
		return;
	}
	coprocessor_manager_get_status(&st);

	if (dtr != 0U) {
		dtr_low_since = 0;
		if (!dtr_high && st.uart_mode == COPROCESSOR_UART_CONSOLE) {
			int rc = coprocessor_manager_set_mode(COPROCESSOR_UART_USB_BRIDGE);

			dtr_bridge = rc == 0;
			LOG_INF("USB host opened the CDC: bridge %s (%d)", rc == 0 ? "on" : "refused",
				rc);
		}
		dtr_high = true;
		return;
	}

	dtr_high = false;
	if (!dtr_bridge || st.uart_mode != COPROCESSOR_UART_USB_BRIDGE) {
		dtr_bridge = false;
		return;
	}
	if (dtr_low_since == 0) {
		dtr_low_since = now;
	} else if (now - dtr_low_since >= DTR_RELEASE_MS) {
		int rc = coprocessor_manager_set_mode(COPROCESSOR_UART_CONSOLE);

		LOG_INF("USB host closed the CDC: console %s (%d)", rc == 0 ? "back" : "refused", rc);
		dtr_bridge = rc != 0;
		dtr_low_since = 0;
	}
}

/* esptool and friends change the rate after syncing; the bridge follows. */
static void follow_baud(void)
{
	uint32_t baud = 0;
	struct uart_config cfg;

	if (atomic_get(&attached) != COPROCESSOR_UART_USB_BRIDGE) {
		bridge_baud = 0;
		return;
	}
	if (uart_line_ctrl_get(cdc, UART_LINE_CTRL_BAUD_RATE, &baud) != 0 || baud == 0U ||
	    baud == bridge_baud) {
		return;
	}
	if (uart_config_get(uart, &cfg) == 0) {
		cfg.baudrate = baud;
		if (uart_configure(uart, &cfg) == 0) {
			bridge_baud = baud;
		}
	}
}

static void worker(void *p1, void *p2, void *p3)
{
	int64_t next_dtr = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		bool banner;

		(void)k_sem_take(&wake, K_MSEC(POLL_MS));
		const int64_t now = k_uptime_get();

		k_mutex_lock(&asm_lock, K_FOREVER);
		drain_console(now);
		esp32_log_poll(&assembler, now);
		banner = banner_seen;
		banner_seen = false;
		k_mutex_unlock(&asm_lock);

		if (banner) {
			coprocessor_manager_note_banner();
		}
		if (now >= next_dtr) {
			follow_dtr(now);
			follow_baud();
			next_dtr = now + DTR_POLL_MS;
		}
	}
}

/* -- start ------------------------------------------------------------------------------- */

int coprocessor_service_start(void)
{
	int rc;

	if (!device_is_ready(uart) || !device_is_ready(cdc)) {
		LOG_ERR("USART3 or its CDC is not ready");
		return -ENODEV;
	}
	console_config_valid = uart_config_get(uart, &console_config) == 0;

	esp32_log_init(&assembler, emit_line, NULL, CONFIG_ESP32_LOG_SOURCE_IDLE_FLUSH_MS);

	uart_irq_callback_user_data_set(cdc, cdc_isr, NULL);
	uart_irq_rx_enable(cdc);

	rc = coprocessor_manager_init(&platform);
	if (rc != 0) {
		LOG_ERR("coprocessor manager: %d", rc);
	}

	k_thread_create(&worker_thread, coprocessor_worker_stack, WORKER_STACK_SIZE, worker, NULL,
			NULL, NULL, WORKER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&worker_thread, "coprocessor");
	started = true;

	return rc;
}

bool coprocessor_service_rx_seen(void)
{
	return atomic_get(&rx_bytes) > 0;
}

#if defined(CONFIG_WIFI_ESP_HOSTED_MCU)
/* drivers/misc/esp_hosted_mcu/esp_hosted_mcu.h is private to the driver. */
uint32_t esp_hosted_mcu_fw_version(void);
#endif

bool coprocessor_service_firmware_version(char *buf, size_t cap)
{
#if defined(CONFIG_WIFI_ESP_HOSTED_MCU)
	const uint32_t v = esp_hosted_mcu_fw_version();

	if (v == 0U) {
		return false;
	}
	/* Packed as (major << 16) | (minor << 8) | patch by the coprocessor. */
	(void)snprintf(buf, cap, "v%u.%u.%u", (unsigned int)((v >> 16) & 0xff),
		       (unsigned int)((v >> 8) & 0xff), (unsigned int)(v & 0xff));
	return true;
#else
	ARG_UNUSED(buf);
	ARG_UNUSED(cap);
	return false;
#endif
}

/* -- shell ------------------------------------------------------------------------------ */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct coprocessor_status st;
	struct esp32_log_stats a;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	coprocessor_manager_get_status(&st);
	k_mutex_lock(&asm_lock, K_FOREVER);
	a = assembler.stats;
	k_mutex_unlock(&asm_lock);

	shell_print(sh, "uart_mode %s, generation %u, transport %s, dtr follow %s",
		    coprocessor_uart_mode_str(st.uart_mode), st.generation,
		    st.transport_ready ? "ready" : "down", atomic_get(&dtr_follow) ? "on" : "off");
	shell_print(sh, "switches %u, rx stop failures %u, last switch error %d, resets %u "
			"(unexpected %u)",
		    st.switches, st.rx_stop_failures, st.last_switch_error, st.resets,
		    st.unexpected_resets);
	shell_print(sh, "rx bytes %ld, rx activity %ld, console ring overflows %ld (%ld bytes lost, "
			"%ld pending)",
		    (long)atomic_get(&rx_bytes), (long)atomic_get(&rx_activity),
		    (long)atomic_get(&console_overflow_episodes),
		    (long)atomic_get(&console_bytes_lost), (long)atomic_get(&console_overflows));
	shell_print(sh, "bridge dropped to host %ld, to chip %ld; host bytes discarded %ld; "
			"host TX wakeups %ld",
		    (long)atomic_get(&dropped_to_host), (long)atomic_get(&dropped_to_chip),
		    (long)atomic_get(&host_bytes_discarded), (long)atomic_get(&host_tx_wakeups));
	shell_print(sh, "lines %u, truncated %u, replacements %u, escapes %u, idle flushes %u, "
			"overflows %u",
		    a.lines, a.truncated_lines, a.replacements, a.escape_sequences, a.idle_flushes,
		    a.overflows);
	return 0;
}

static int cmd_mode(const struct shell *sh, size_t argc, char **argv)
{
	enum coprocessor_uart_mode mode;
	int64_t t0;
	int rc;

	if (argc != 2) {
		shell_error(sh, "usage: coproc mode console|bridge|flashing");
		return -EINVAL;
	}
	if (strcmp(argv[1], "console") == 0) {
		mode = COPROCESSOR_UART_CONSOLE;
	} else if (strcmp(argv[1], "bridge") == 0) {
		mode = COPROCESSOR_UART_USB_BRIDGE;
	} else if (strcmp(argv[1], "flashing") == 0) {
		mode = COPROCESSOR_UART_FLASHING;
	} else {
		shell_error(sh, "unknown mode %s", argv[1]);
		return -EINVAL;
	}
	t0 = k_uptime_get();
	rc = coprocessor_manager_set_mode(mode);
	shell_print(sh, "coproc mode %s: %d in %lld ms", argv[1], rc, k_uptime_get() - t0);
	return rc;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	const bool download = argc > 1 && strcmp(argv[1], "download") == 0;
	int rc = coprocessor_manager_reset(download);

	shell_print(sh, "coproc reset%s: %d", download ? " download" : "", rc);
	return rc;
}

static int cmd_dtr(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2) {
		atomic_set(&dtr_follow, strcmp(argv[1], "on") == 0 ? 1 : 0);
	}
	shell_print(sh, "dtr follow %s", atomic_get(&dtr_follow) ? "on" : "off");
	return 0;
}

static int cmd_logstore(const struct shell *sh, size_t argc, char **argv)
{
	struct log_store_timing t;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (int s = 0; s < LOG_STORE_SOURCE_COUNT; s++) {
		struct log_store_stats st;

		log_store_get_stats((enum log_store_source)s, &st);
		shell_print(sh,
			    "%s: records %u, used %u of %u B, appended %llu, overwritten %llu, "
			    "lost backend %llu, lost uart %llu",
			    log_store_source_str((enum log_store_source)s), st.records, st.used_bytes,
			    st.capacity_bytes, (unsigned long long)st.appended,
			    (unsigned long long)st.overwritten,
			    (unsigned long long)st.lost[LOG_STORE_LOSS_BACKEND],
			    (unsigned long long)st.lost[LOG_STORE_LOSS_UART]);
	}
	log_store_get_timing(&t);
	shell_print(sh, "lock: append hold max %u us, append wait max %u us, read hold max %u us",
		    t.append_hold_max_us, t.append_wait_max_us, t.read_hold_max_us);
	shell_print(sh, "next seq %llu", (unsigned long long)log_store_next_seq());
	return 0;
}

/*
 * Debug: a burst of STM32 log messages for the bench (reports/p5/hw). `log enable dbg`
 * does not produce one on this build - modules are compiled at the default level, so
 * their debug calls are not in the image. Runs on the calling shell's thread.
 */
static int cmd_burst(const struct shell *sh, size_t argc, char **argv)
{
	/* Bounded: the console drops input bytes, and a lost space once turned
	 * "1000 120" into a burst of 1000120 that held this shell for minutes. */
	const long n = argc > 1 ? CLAMP(strtol(argv[1], NULL, 10), 1, 100000) : 1000;
	const long len = argc > 2 ? CLAMP(strtol(argv[2], NULL, 10), 1, 600) : 80;
	static char text[601];
	const int64_t t0 = k_uptime_get();

	memset(text, 'b', (size_t)len);
	text[len] = '\0';
	for (long i = 0; i < n; i++) {
		LOG_INF("burst %ld of %ld %s", i + 1, n, text);
	}
	shell_print(sh, "coproc burst: %ld messages of %ld bytes in %lld ms", n, len,
		    k_uptime_get() - t0);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(coproc_cmds,
	SHELL_CMD(burst, NULL, "Debug: <n> [len] STM32 log messages as fast as possible.", cmd_burst),
	SHELL_CMD(status, NULL, "UART owner, generation, counters.", cmd_status),
	SHELL_CMD(mode, NULL, "Hand USART3 to console|bridge|flashing (stand-in).", cmd_mode),
	SHELL_CMD(reset, NULL, "Reset the C6 through EN; 'download' holds BOOT.", cmd_reset),
	SHELL_CMD(dtr, NULL, "Follow DTR on the CDC: on|off.", cmd_dtr),
	SHELL_CMD(logs, NULL, "log-store rings, losses and lock times.", cmd_logstore),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(coproc, &coproc_cmds, "ESP32-C6 UART owner and logs (P5).", NULL);
