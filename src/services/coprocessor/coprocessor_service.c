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
#if defined(CONFIG_FIRMWARE_STORE)
#include <zephyr/fs/fs.h>

#include <firmware_store/firmware_store.h>
#endif
#if defined(CONFIG_COPROCESSOR_UPDATER)
#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>

#include <coprocessor_updater/coprocessor_updater.h>
#include <esp_loader_adapter/esp_loader_adapter.h>
#include <esp_loader_adapter/updater_loader.h>
#endif

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

/* The staged coprocessor image (firmware-store); the updater's journal lives here too. */
#define FIRMWARE_DIR      "/lfs/firmware"
/* An upload untouched for a day is removed on the next tick; once a minute is plenty. */
#define FIRMWARE_TICK_MS  60000

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

#if defined(CONFIG_COPROCESSOR_UPDATER)
/*
 * The updater's early sign that the new firmware runs: ESP-IDF's application
 * start as the console assembles it after the normal boot. Recorded next to the
 * transport's answer, never decisive on its own (owner's decision №2).
 * Written on the worker under asm_lock, read by the updater's thread.
 */
static struct k_spinlock evidence_lock;
static bool evidence_seen;
static char evidence_version[COPROCESSOR_UPDATE_VERSION_MAX_LEN + 1];

static bool tag_is(const struct esp32_log_line *line, const char *tag)
{
	size_t n = strlen(tag);

	return line->module != NULL && line->module_len == n && memcmp(line->module, tag, n) == 0;
}

static bool text_starts(const struct esp32_log_line *line, const char *prefix)
{
	size_t n = strlen(prefix);

	return line->text_len >= n && memcmp(line->text, prefix, n) == 0;
}

static void note_boot_evidence(const struct esp32_log_line *line)
{
	static const char version_prefix[] = "App version:";

	if (tag_is(line, "app_init") && text_starts(line, version_prefix)) {
		const char *v = line->text + sizeof(version_prefix) - 1U;
		size_t n = line->text_len - (sizeof(version_prefix) - 1U);

		while (n > 0U && *v == ' ') {
			v++;
			n--;
		}
		n = MIN(n, sizeof(evidence_version) - 1U);

		K_SPINLOCK(&evidence_lock) {
			memcpy(evidence_version, v, n);
			evidence_version[n] = '\0';
			evidence_seen = true;
		}
	} else if ((tag_is(line, "main_task") && text_starts(line, "Calling app_main")) ||
		   (tag_is(line, "app_init") && text_starts(line, "Project name:"))) {
		K_SPINLOCK(&evidence_lock) {
			evidence_seen = true;
		}
	}
}
#endif

static void emit_line(void *ctx, const struct esp32_log_line *line)
{
	ARG_UNUSED(ctx);

#if defined(CONFIG_COPROCESSOR_UPDATER)
	note_boot_evidence(line);
#endif

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

/* -- the install: esp-loader-adapter's board steps and coprocessor-updater's platform -- */

#if defined(CONFIG_COPROCESSOR_UPDATER)
#if defined(CONFIG_WIFI_ESP_HOSTED_MCU)
/* The restart patch (patches/zephyr/esp_hosted_mcu-restart-after-coprocessor-flash.patch). */
int esp_hosted_mcu_restart(k_timeout_t timeout);
int esp_hosted_mcu_wifi_restart(void);
/* patches/zephyr/esp_hosted_mcu-suspend-while-coprocessor-flashes.patch */
void esp_hosted_mcu_suspend(bool suspend);
#endif

#define JOURNAL_PATH     FIRMWARE_DIR "/update.journal"
#define JOURNAL_TMP_PATH FIRMWARE_DIR "/update.journal.tmp"
#define JOURNAL_MAGIC    0x4a504350U /* "PCPJ" */

/*
 * The C6 must be quiet whenever esp-serial-flasher's port is up. The port's
 * tty_serial receives USART3 into a 512-byte ring and, once it is full, writes "~"
 * from the ISR with an unbounded wait (zephyr/subsys/console/tty.c; the library
 * sets a finite tx timeout only inside its own writes). An unflashed C6 prints its
 * ROM loop at ~11 KB/s, so a preemption of ~50 ms between the port's init and the
 * library's first read - the W5500's cooperative thread busy-waits ~200 ms on each
 * socket reopen - fills the ring and stops the STM32: board B hung that way at boot
 * (hw/logs/02) and in the owner's first install through the web (hw/logs/08). The
 * same window follows reset_target, when the booting firmware prints its log before
 * the port is deinitialised. Therefore: the C6 is put into its ROM download mode (a
 * short banner, then silence) before the port's init, and reset_target only records
 * that a normal boot is due; lines_idle(), called after the port's deinit, releases it.
 */
static int (*lib_port_init)(void *ctx);
static bool boot_after_deinit;

static int quiet_port_init(void *ctx)
{
#if defined(CONFIG_WIFI_ESP_HOSTED_MCU)
	/*
	 * In its ROM loader the C6 holds IO23 (data-ready, PC9) high. ESP-Hosted's
	 * receive thread takes that level for "frames queued" and polls the shared
	 * SPI2 without pause: board B's STM32 spent the owner's second install in
	 * spi_stm32 under esp_hosted_mcu_event_task, with the W5500 and HTTP starved
	 * and the updater stuck in entering_bootloader (hw/logs/09, two J-Link halts).
	 * The transport stays suspended for the whole session.
	 */
	esp_hosted_mcu_suspend(true);
#endif
	(void)op_reset(NULL, true);
	boot_after_deinit = false;
	return lib_port_init(ctx);
}

static void deferred_reset_target(void *ctx)
{
	ARG_UNUSED(ctx);
	boot_after_deinit = true;
}

/*
 * What each library call returned and how long it took, for `coproc updater`. The
 * adapter logs a failure, but the deferred log dropped every line of the first
 * install that reached `writing` (20 s of silence on the console, hw/logs/11), so
 * the codes are kept here where a full log buffer cannot lose them.
 */
static struct {
	int connect_rc;
	uint32_t connect_ms;
	int start_rc;
	uint32_t start_ms;
	uint32_t writes;
	int write_rc;
	uint32_t write_ms_last;
	uint32_t write_ms_max;
	uint32_t writes_over_250ms;
	uint32_t writes_over_500ms;
	/* Debug (hw row 18, worker ~107 s CPU per install): where the worker's time goes. */
	uint32_t image_reads;
	int64_t image_read_ticks;
	uint64_t image_read_cpu;
	int64_t write_ticks;
	uint64_t write_cpu;
	int target;
	int finish_rc;
	uint32_t finish_ms;
} flash_stats;

/*
 * Debug (reports/p6 hw, attempt 5): CPU time of every thread from flash_start to
 * the first failed write or to finish. A block's 1000 ms ran out with the ROM
 * silent; whoever held the processor shows up here. The ISR time lands on the
 * thread it interrupted.
 */
#define CPU_THREADS_MAX 48

static struct {
	k_tid_t tid;
	uint64_t cycles;
	uint64_t delta;
} cpu_threads[CPU_THREADS_MAX];
static size_t cpu_threads_n;
static uint64_t cpu_all_start;
static uint64_t cpu_idle_start;
static uint64_t cpu_all_delta;
static uint64_t cpu_idle_delta;
static bool cpu_window_open;

static void cpu_thread_start(const struct k_thread *thread, void *user)
{
	k_thread_runtime_stats_t s;

	ARG_UNUSED(user);
	if (cpu_threads_n < CPU_THREADS_MAX &&
	    k_thread_runtime_stats_get((k_tid_t)thread, &s) == 0) {
		cpu_threads[cpu_threads_n].tid = (k_tid_t)thread;
		cpu_threads[cpu_threads_n].cycles = s.execution_cycles;
		cpu_threads[cpu_threads_n].delta = 0;
		cpu_threads_n++;
	}
}

static void cpu_thread_end(const struct k_thread *thread, void *user)
{
	k_thread_runtime_stats_t s;

	ARG_UNUSED(user);
	if (k_thread_runtime_stats_get((k_tid_t)thread, &s) != 0) {
		return;
	}
	for (size_t i = 0; i < cpu_threads_n; i++) {
		if (cpu_threads[i].tid == (k_tid_t)thread) {
			cpu_threads[i].delta = s.execution_cycles - cpu_threads[i].cycles;
			return;
		}
	}
}

static void cpu_window_begin(void)
{
	k_thread_runtime_stats_t all;

	cpu_threads_n = 0;
	k_thread_foreach_unlocked(cpu_thread_start, NULL);
	(void)k_thread_runtime_stats_all_get(&all);
	cpu_all_start = all.execution_cycles;
	cpu_idle_start = all.idle_cycles;
	cpu_all_delta = 0;
	cpu_idle_delta = 0;
	cpu_window_open = true;
}

static void cpu_window_end(void)
{
	k_thread_runtime_stats_t all;

	if (!cpu_window_open) {
		return;
	}
	cpu_window_open = false;
	k_thread_foreach_unlocked(cpu_thread_end, NULL);
	(void)k_thread_runtime_stats_all_get(&all);
	cpu_all_delta = all.execution_cycles - cpu_all_start;
	cpu_idle_delta = all.idle_cycles - cpu_idle_start;
}

static uint32_t cycles_ms(uint64_t cycles)
{
	return (uint32_t)(cycles * 1000U / (uint64_t)sys_clock_hw_cycles_per_sec());
}

/* CPU cycles the calling thread has run so far (0 when the stats are off). */
static uint64_t own_cycles(void)
{
	k_thread_runtime_stats_t s;

	return k_thread_runtime_stats_get(k_current_get(), &s) == 0 ? s.execution_cycles : 0U;
}

static int (*lib_connect)(void *ctx);
static int (*lib_flash_start)(void *ctx, uint32_t offset, uint32_t size, uint32_t block_size);
static int (*lib_flash_write)(void *ctx, const uint8_t *data, uint32_t len);
static int (*lib_flash_finish)(void *ctx);

static int timed_connect(void *ctx)
{
	const int64_t t0 = k_uptime_get();

	memset(&flash_stats, 0, sizeof(flash_stats));
	flash_stats.target = -1;
	/* The window is the whole session: attempt 6 lost ~600 ms while connecting (hw/logs/13). */
	esp_loader_adapter_zephyr_io_reset();
	cpu_window_begin();
	flash_stats.connect_rc = lib_connect(ctx);
	flash_stats.connect_ms = (uint32_t)(k_uptime_get() - t0);
	return flash_stats.connect_rc;
}

static int (*lib_get_target)(void *ctx);

static int recorded_get_target(void *ctx)
{
	flash_stats.target = lib_get_target(ctx);
	return flash_stats.target;
}

static int timed_flash_start(void *ctx, uint32_t offset, uint32_t size, uint32_t block_size)
{
	const int64_t t0 = k_uptime_get();

	flash_stats.start_rc = lib_flash_start(ctx, offset, size, block_size);
	flash_stats.start_ms = (uint32_t)(k_uptime_get() - t0);
	if (flash_stats.start_rc != 0) {
		cpu_window_end();
	}
	return flash_stats.start_rc;
}

static int timed_flash_write(void *ctx, const uint8_t *data, uint32_t len)
{
	const int64_t t0 = k_uptime_get();
	const int64_t w0 = k_uptime_ticks();
	const uint64_t c0 = own_cycles();
	const int rc = lib_flash_write(ctx, data, len);
	const uint32_t ms = (uint32_t)(k_uptime_get() - t0);

	flash_stats.write_ticks += k_uptime_ticks() - w0;
	flash_stats.write_cpu += own_cycles() - c0;
	flash_stats.writes++;
	flash_stats.write_rc = rc;
	flash_stats.write_ms_last = ms;
	flash_stats.write_ms_max = MAX(flash_stats.write_ms_max, ms);
	flash_stats.writes_over_250ms += ms > 250U ? 1U : 0U;
	flash_stats.writes_over_500ms += ms > 500U ? 1U : 0U;
	if (rc != 0) {
		cpu_window_end();
	}
	return rc;
}

static int timed_flash_finish(void *ctx)
{
	const int64_t t0 = k_uptime_get();

	flash_stats.finish_rc = lib_flash_finish(ctx);
	flash_stats.finish_ms = (uint32_t)(k_uptime_get() - t0);
	cpu_window_end();
	return flash_stats.finish_rc;
}

/* EN and BOOT back to inactive outputs (esp_loader_deinit() leaves them floating),
 * with the normal boot reset_target asked for, now that the port is down. */
static int lines_idle(void *ctx)
{
	int rc;

	ARG_UNUSED(ctx);
	cpu_window_end();
	if (boot_after_deinit) {
		boot_after_deinit = false;
		/* Leaves both lines inactive outputs after the EN pulse, BOOT released. */
		rc = op_reset(NULL, false);
	} else {
		rc = gpio_pin_configure_dt(&en, GPIO_OUTPUT_INACTIVE);
		rc = rc ? rc : gpio_pin_configure_dt(&boot, GPIO_OUTPUT_INACTIVE);
	}
#if defined(CONFIG_WIFI_ESP_HOSTED_MCU)
	/* The C6 left its ROM loader: ESP-Hosted may read data-ready again. */
	esp_hosted_mcu_suspend(false);
#endif
	return rc;
}

static int console_restore(void *ctx)
{
	ARG_UNUSED(ctx);
	restore_console_config();
	return 0;
}

static struct esp_loader_adapter_lib loader_lib;

static char image_upload[COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN + 1];

/* The install holds the staged file for as long as it reads it: no delete, no expiry. */
static int up_image_open(void *ctx, const char *upload_id, uint32_t *size)
{
	int rc;

	ARG_UNUSED(ctx);
	rc = fw_store_set_in_use(upload_id, true);
	if (rc != 0) {
		return rc;
	}
	rc = fw_store_image_open(upload_id, size);
	if (rc != 0) {
		(void)fw_store_set_in_use(upload_id, false);
		return rc;
	}
	strncpy(image_upload, upload_id, sizeof(image_upload) - 1U);
	image_upload[sizeof(image_upload) - 1U] = '\0';

	return 0;
}

static int up_image_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
{
	const int64_t t0 = k_uptime_ticks();
	const uint64_t c0 = own_cycles();
	int rc;

	ARG_UNUSED(ctx);
	rc = fw_store_image_read(offset, buf, len);
	flash_stats.image_reads++;
	flash_stats.image_read_ticks += k_uptime_ticks() - t0;
	flash_stats.image_read_cpu += own_cycles() - c0;
	return rc;
}

static void up_image_close(void *ctx)
{
	ARG_UNUSED(ctx);
	fw_store_image_close();
	if (image_upload[0] != '\0') {
		(void)fw_store_set_in_use(image_upload, false);
		image_upload[0] = '\0';
	}
}

struct journal_file {
	uint32_t magic;
	uint32_t size;
	uint32_t crc;
	struct coprocessor_update_journal journal;
};

static int up_journal_load(void *ctx, struct coprocessor_update_journal *out)
{
	static struct journal_file f;
	struct fs_file_t file;
	ssize_t n;
	int rc;

	ARG_UNUSED(ctx);
	fs_file_t_init(&file);
	rc = fs_open(&file, JOURNAL_PATH, FS_O_READ);
	if (rc != 0) {
		return -ENOENT;
	}
	n = fs_read(&file, &f, sizeof(f));
	(void)fs_close(&file);
	if (n != (ssize_t)sizeof(f) || f.magic != JOURNAL_MAGIC || f.size != sizeof(f.journal) ||
	    f.crc != crc32_ieee((const uint8_t *)&f.journal, sizeof(f.journal))) {
		LOG_WRN("update journal unreadable; treated as none");
		return -ENOENT;
	}
	*out = f.journal;

	return 0;
}

/* Written beside the old one, synced, then renamed over it: a power cut leaves either. */
static int up_journal_save(void *ctx, const struct coprocessor_update_journal *journal)
{
	static struct journal_file f;
	struct fs_file_t file;
	ssize_t n;
	int rc;

	ARG_UNUSED(ctx);
	f.magic = JOURNAL_MAGIC;
	f.size = sizeof(f.journal);
	f.journal = *journal;
	f.crc = crc32_ieee((const uint8_t *)&f.journal, sizeof(f.journal));

	fs_file_t_init(&file);
	rc = fs_open(&file, JOURNAL_TMP_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return rc;
	}
	n = fs_write(&file, &f, sizeof(f));
	rc = fs_sync(&file);
	(void)fs_close(&file);
	if (n != (ssize_t)sizeof(f) || rc != 0) {
		(void)fs_unlink(JOURNAL_TMP_PATH);
		return n < 0 ? (int)n : -EIO;
	}
	rc = fs_rename(JOURNAL_TMP_PATH, JOURNAL_PATH);
	if (rc != 0) {
		(void)fs_unlink(JOURNAL_PATH);
		rc = fs_rename(JOURNAL_TMP_PATH, JOURNAL_PATH);
	}

	return rc;
}

static void up_boot_evidence_arm(void *ctx)
{
	ARG_UNUSED(ctx);
	K_SPINLOCK(&evidence_lock) {
		evidence_seen = false;
		evidence_version[0] = '\0';
	}
}

static bool up_boot_evidence(void *ctx, char *app_version, size_t cap)
{
	bool seen = false;

	ARG_UNUSED(ctx);
	K_SPINLOCK(&evidence_lock) {
		seen = evidence_seen;
		if (cap > 0U) {
			strncpy(app_version, evidence_version, cap - 1U);
			app_version[cap - 1U] = '\0';
		}
	}

	return seen;
}

static int up_transport_restart(void *ctx, uint32_t timeout_ms, char *version, size_t cap)
{
	ARG_UNUSED(ctx);
	if (cap > 0U) {
		version[0] = '\0';
	}
#if defined(CONFIG_WIFI_ESP_HOSTED_MCU)
	int rc = esp_hosted_mcu_restart(K_MSEC(timeout_ms));

	if (rc != 0) {
		/* -ENODEV here is the core that never came up: no answer either way. */
		return -ETIMEDOUT;
	}
	(void)coprocessor_service_firmware_version(version, cap);

	rc = esp_hosted_mcu_wifi_restart();
	if (rc != 0) {
		LOG_WRN("Wi-Fi restart after the install: %d", rc);
	}
	return rc;
#else
	ARG_UNUSED(timeout_ms);
	return -ETIMEDOUT;
#endif
}

static int64_t up_now(void *ctx)
{
	ARG_UNUSED(ctx);
	return k_uptime_get();
}

static void up_sleep(void *ctx, uint32_t ms)
{
	ARG_UNUSED(ctx);
	k_msleep(ms);
}

static struct coprocessor_updater_platform updater_platform = {
	.image_open = up_image_open,
	.image_read = up_image_read,
	.image_close = up_image_close,
	.journal_load = up_journal_load,
	.journal_save = up_journal_save,
	.boot_evidence_arm = up_boot_evidence_arm,
	.boot_evidence = up_boot_evidence,
	.transport_restart = up_transport_restart,
	.now_ms = up_now,
	.sleep_ms = up_sleep,
};
#endif /* CONFIG_COPROCESSOR_UPDATER */

static bool firmware_ready;

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
	int64_t next_firmware_tick = FIRMWARE_TICK_MS;

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
#if defined(CONFIG_FIRMWARE_STORE)
		/* Takes the store's lock only; file I/O stays on the API's job worker. */
		if (now >= next_firmware_tick) {
			fw_store_tick(now);
			next_firmware_tick = now + FIRMWARE_TICK_MS;
		}
#endif
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

#if defined(CONFIG_FIRMWARE_STORE)
	/* /lfs is mounted by fstab before main() runs (see main.c). A store that does
	 * not open leaves the upload operations answering 503, not the service down. */
	int fw_rc = fw_store_init(FIRMWARE_DIR, k_uptime_get());

	if (fw_rc != 0) {
		LOG_ERR("firmware store in %s: %d", FIRMWARE_DIR, fw_rc);
	}
	firmware_ready = fw_rc == 0;
#endif

#if defined(CONFIG_COPROCESSOR_UPDATER)
	/* The install needs the store (image and journal live in its directory). */
	if (firmware_ready) {
		int up_rc;

		esp_loader_adapter_zephyr_fill(&loader_lib);
		/* Keep the C6 quiet while the port is up (see quiet_port_init()). */
		lib_port_init = loader_lib.port_init;
		loader_lib.port_init = quiet_port_init;
		lib_connect = loader_lib.connect;
		loader_lib.connect = timed_connect;
		lib_get_target = loader_lib.get_target;
		loader_lib.get_target = recorded_get_target;
		lib_flash_start = loader_lib.flash_start;
		loader_lib.flash_start = timed_flash_start;
		lib_flash_write = loader_lib.flash_write;
		loader_lib.flash_write = timed_flash_write;
		lib_flash_finish = loader_lib.flash_finish;
		loader_lib.flash_finish = timed_flash_finish;
		loader_lib.reset_target = deferred_reset_target;
		loader_lib.lines_idle = lines_idle;
		loader_lib.console_restore = console_restore;
		up_rc = esp_loader_adapter_init(&loader_lib);
		updater_platform.loader = esp_loader_adapter_updater_loader();
		up_rc = up_rc ? up_rc : coprocessor_updater_init(&updater_platform);
		if (up_rc != 0) {
			LOG_ERR("coprocessor updater: %d", up_rc);
		}
	}
#endif

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

bool coprocessor_service_firmware_ready(void)
{
	return firmware_ready;
}

bool coprocessor_service_request_over_ethernet(uint8_t family, const uint8_t *addr)
{
	return net_adapter_is_ethernet_address(family, addr);
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

#if defined(CONFIG_FIRMWARE_STORE)
/*
 * Debug: the staged upload survives a reboot, and the API names it only to the client
 * that created it. A bench script that died mid-upload left one on board B, which then
 * refused every new upload with 409 busy (reports/p6). `coproc upload drop` removes
 * the store's files and reopens it. Refused while an install runs.
 */
static int cmd_upload(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const files[] = {
		FIRMWARE_DIR "/upload.bin",
		FIRMWARE_DIR "/upload.meta",
		FIRMWARE_DIR "/upload.meta.tmp",
	};
	int rc;

	if (argc != 2 || strcmp(argv[1], "drop") != 0) {
		shell_error(sh, "usage: coproc upload drop");
		return -EINVAL;
	}
#if defined(CONFIG_COPROCESSOR_UPDATER)
	if (coprocessor_updater_check() != 0) {
		shell_error(sh, "coproc upload: refused, an install is active or the bridge owns the UART");
		return -EBUSY;
	}
#endif
	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		shell_print(sh, "coproc upload: unlink %s: %d", files[i], fs_unlink(files[i]));
	}
	rc = fw_store_init(FIRMWARE_DIR, k_uptime_get());
	firmware_ready = rc == 0;
	shell_print(sh, "coproc upload: store reopened: %d", rc);
	return rc;
}
#endif

#if defined(CONFIG_COPROCESSOR_UPDATER)
/* Debug: the updater's journal, including the upload an interrupted install used. */
static int cmd_updater(const struct shell *sh, size_t argc, char **argv)
{
	struct coprocessor_updater_state st;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	coprocessor_updater_get_state(&st);
	shell_print(sh, "updater: active %s, phase %s, job %s, upload %s", st.active ? "yes" : "no",
		    coprocessor_update_phase_str(st.phase), st.job_id[0] ? st.job_id : "-",
		    st.upload_id[0] ? st.upload_id : "-");
	if (st.has_last) {
		shell_print(sh, "updater: last %s %s, version %s, recovery_required %s, error %s: %s",
			    st.last.job_id, coprocessor_update_outcome_str(st.last.state),
			    st.last.version[0] ? st.last.version : "-",
			    st.last.recovery_required ? "yes" : "no",
			    st.last.has_error ? st.last.error_code : "-",
			    st.last.has_error ? st.last.error_message : "-");
	}
	shell_print(sh,
		    "library: connect %d in %u ms, flash_start %d in %u ms, writes %u (last %d in "
		    "%u ms, max %u ms), finish %d in %u ms",
		    flash_stats.connect_rc, flash_stats.connect_ms, flash_stats.start_rc,
		    flash_stats.start_ms, flash_stats.writes, flash_stats.write_rc,
		    flash_stats.write_ms_last, flash_stats.write_ms_max, flash_stats.finish_rc,
		    flash_stats.finish_ms);
	shell_print(sh, "library: target %d, writes over 250 ms %u, over 500 ms %u",
		    flash_stats.target, flash_stats.writes_over_250ms,
		    flash_stats.writes_over_500ms);

	struct esp_loader_adapter_zephyr_io io;

	esp_loader_adapter_zephyr_io_get(&io);
	shell_print(sh,
		    "worker: image reads %u in %u ms wall, %u ms own CPU; library writes %u ms wall, "
		    "%u ms own CPU",
		    flash_stats.image_reads,
		    (uint32_t)k_ticks_to_ms_floor64(flash_stats.image_read_ticks),
		    cycles_ms(flash_stats.image_read_cpu),
		    (uint32_t)k_ticks_to_ms_floor64(flash_stats.write_ticks),
		    cycles_ms(flash_stats.write_cpu));
	shell_print(sh, "port: longest read past its timeout %u ms, at uptime %lld ms",
		    io.overrun_max_ms, (long long)io.overrun_at_ms);

	/* Last bytes in, oldest first; "|" where the library wrote. */
	char tail[3 * ARRAY_SIZE(io.rx_tail) + 1];
	size_t len = 0;

	for (size_t i = 0; i < io.rx_tail_count; i++) {
		size_t at = (io.rx_tail_next + ARRAY_SIZE(io.rx_tail) - io.rx_tail_count + i) %
			    ARRAY_SIZE(io.rx_tail);
		uint16_t v = io.rx_tail[at];

		len += snprintf(&tail[len], sizeof(tail) - len, v > 0xFFU ? "| " : "%02x ",
				(unsigned int)v);
	}
	tail[len] = '\0';
	shell_print(sh, "port: rx tail %s", tail);
	shell_print(sh,
		    "port: reads %u in %u ms (max %u, last failure %d after %u of %u ms, %u bytes "
		    "since write), writes %u in %u ms (max %u, last failure %d after %u of %u ms)",
		    io.reads, (uint32_t)k_ticks_to_ms_floor64(io.read_ticks), io.read_max_ms,
		    io.read_rc, io.read_fail_ms, io.read_fail_timeout_ms, io.read_fail_bytes, io.writes,
		    (uint32_t)k_ticks_to_ms_floor64(io.write_ticks), io.write_max_ms, io.write_rc,
		    io.write_fail_ms, io.write_fail_timeout_ms);

	if (cpu_window_open) {
		shell_print(sh, "cpu: window still open (no failed write or finish yet)");
		return 0;
	}
	shell_print(sh, "cpu: window %u ms, idle %u ms, %u threads", cycles_ms(cpu_all_delta),
		    cycles_ms(cpu_idle_delta), (uint32_t)cpu_threads_n);
	/* Top threads by CPU time over the window, largest first, selection by hand. */
	bool shown[CPU_THREADS_MAX] = {false};

	for (int k = 0; k < 10; k++) {
		size_t best = cpu_threads_n;

		for (size_t i = 0; i < cpu_threads_n; i++) {
			if (!shown[i] && (best == cpu_threads_n ||
					  cpu_threads[i].delta > cpu_threads[best].delta)) {
				best = i;
			}
		}
		if (best == cpu_threads_n || cpu_threads[best].delta == 0U) {
			break;
		}
		shown[best] = true;
		const char *name = k_thread_name_get(cpu_threads[best].tid);

		shell_print(sh, "cpu: %-24s %u ms", name != NULL && name[0] ? name : "?",
			    cycles_ms(cpu_threads[best].delta));
	}
	return 0;
}

/*
 * Debug, read-only for the C6 (reports/p6 hw step 2): take the UART through
 * esp-loader-adapter, enter the ROM loader, connect and check the chip, then close
 * the session - normal boot, EN/BOOT idle, console back. No write, erase or begin.
 * Refused while an install runs; blocks this shell for about a second.
 */
static int cmd_loader(const struct shell *sh, size_t argc, char **argv)
{
	struct esp_loader_adapter_error err = {0};
	struct coprocessor_status st;
	int64_t t0;
	int64_t opened_ms;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (coprocessor_updater_check() != 0) {
		shell_error(sh, "coproc loader: refused, an install is active or the bridge owns the UART");
		return -EBUSY;
	}
	t0 = k_uptime_get();
	rc = esp_loader_adapter_open(&err);
	opened_ms = k_uptime_get() - t0;
	if (rc != 0) {
		shell_print(sh, "coproc loader: open %d in %lld ms (%s: %s, cause %d)", rc, opened_ms,
			    err.code ? err.code : "-", err.message ? err.message : "-", err.cause);
	} else {
		shell_print(sh, "coproc loader: ESP32-C6 in its ROM loader after %lld ms", opened_ms);
		rc = esp_loader_adapter_close();
		shell_print(sh, "coproc loader: close %d after %lld ms", rc, k_uptime_get() - t0);
	}
	coprocessor_manager_get_status(&st);
	shell_print(sh, "coproc loader: uart_mode %s, generation %u, session %s",
		    coprocessor_uart_mode_str(st.uart_mode), st.generation,
		    esp_loader_adapter_is_open() ? "open" : "closed");
	return rc;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(coproc_cmds,
	SHELL_CMD(burst, NULL, "Debug: <n> [len] STM32 log messages as fast as possible.", cmd_burst),
	SHELL_CMD(status, NULL, "UART owner, generation, counters.", cmd_status),
	SHELL_CMD(mode, NULL, "Hand USART3 to console|bridge|flashing (stand-in).", cmd_mode),
	SHELL_CMD(reset, NULL, "Reset the C6 through EN; 'download' holds BOOT.", cmd_reset),
	SHELL_CMD(dtr, NULL, "Follow DTR on the CDC: on|off.", cmd_dtr),
	SHELL_CMD(logs, NULL, "log-store rings, losses and lock times.", cmd_logstore),
#if defined(CONFIG_FIRMWARE_STORE)
	SHELL_CMD(upload, NULL, "Debug: 'drop' removes the staged upload's files.", cmd_upload),
#endif
#if defined(CONFIG_COPROCESSOR_UPDATER)
	SHELL_CMD(loader, NULL, "Debug: ROM loader connect and back to normal boot, no write.",
		  cmd_loader),
	SHELL_CMD(updater, NULL, "Debug: updater journal - phase, job, upload, last outcome.",
		  cmd_updater),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(coproc, &coproc_cmds, "ESP32-C6 UART owner and logs (P5).", NULL);
