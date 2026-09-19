/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HLW8032 energy meter (include/energy_meter/energy_meter.h).
 *
 * Receive: async UART with DMA into two 24-byte buffers. A frame takes 55 ms and
 * the chip pauses ~50 ms before the next one; with an rx timeout of 0 the driver
 * reports the bytes on the USART's idle-line interrupt, so an event on a buffer
 * that is not full marks the end of a frame. The callback collects bytes into a
 * 24-byte window, checks a full window with hlw8032_frame_ok(), hands a good frame
 * over and submits the work, and empties the window at every pause - the next
 * frame always starts at its first byte. Nothing waits for the UART.
 *
 * Everything else runs on the module's own work queue, which is also the only
 * caller of the ZMS on the FRAM: ZMS has no thread of its own. The queue's stack
 * and the DMA buffers are .noinit, which stays in SRAM - SPI1 and UART DMA take
 * buffers only from there; this library's .bss (struct zms_fs with its 16 KiB
 * lookup cache) is in PSRAM (src/helpers/psram_sections.ld).
 *
 * Stored is the energy itself, in uWh, as electricity meters keep their energy
 * register: each frame adds the PF pulses since the previous frame (the register's
 * difference modulo 65536) times the energy of one pulse with the coefficients of
 * that moment, so a new coefficient changes only what is counted from then on.
 * The first frame after boot only sets the PF base: the chip starts PF from 0
 * when it loses power, so the base is never stored. Energy since the last save
 * (up to a second) is lost with the power.
 */

#include <energy_meter/energy_meter.h>
#include <settings_registry/settings_registry.h>

#include <string.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/kvss/zms.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>

#include "hlw8032.h"

LOG_MODULE_REGISTER(energy_meter, CONFIG_ENERGY_METER_LOG_LEVEL);

#define ENERGY_ID   2 /* uWh; ID 1 held the raw pulse count in 1.1.3+18 */
#define SECTOR_SIZE 2048
#define WQ_PRIO     10

#define VF ((float)CONFIG_ENERGY_METER_VOLTAGE_DIVIDER / 1000.0f)
#define CF (1000.0f / (float)CONFIG_ENERGY_METER_SHUNT_UOHM)

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(cedar_hlw8032_uart));

static K_THREAD_STACK_DEFINE(wq_stack, 2048);
static struct k_work_q wq;
static uint8_t rx_buf[2][HLW8032_FRAME_LEN] __noinit;

/* Shared with the UART callback and readers, under the spinlock */
static struct k_spinlock lock;
static uint8_t frame[HLW8032_FRAME_LEN];
static struct energy_reading reading;

/* UART callback only */
static uint8_t win[HLW8032_FRAME_LEN];
static size_t win_len;
static uint32_t dropped, uart_errors; /* bytes of frames not taken */

/* Work queue only */
static struct zms_fs fs;
static bool mounted;
static struct hlw8032_regs regs;
static uint16_t last_pf;
static uint64_t energy_uwh, saved;
static double energy_frac; /* below 1 uWh, not stored */

/* Calibration, persistent in settings on the NOR (not on the FRAM) */
static float cal_v, cal_i, cal_e;
static const float cal_one = 1.0f;
static struct setting_range_f32 cal_range = {.min = 0.5f, .max = 2.0f};

#define CAL_KEY(_name, _key, _storage)                                                             \
	SETTING_REGISTRY_DEFINE(_name, .key = _key, .type = SETTING_TYPE_F32,                     \
				.storage = SETTING_STORAGE_OWNED,                                  \
				.persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,   \
				.default_value = &cal_one, .ram_storage = &_storage,               \
				.validate = setting_validate_range_f32, .validate_ctx = &cal_range)

CAL_KEY(energy_cal_voltage, "energy/cal/voltage", cal_v);
CAL_KEY(energy_cal_current, "energy/cal/current", cal_i);
CAL_KEY(energy_cal_energy, "energy/cal/energy", cal_e);

static float cal(const char *key)
{
	struct setting_value v = {.type = SETTING_TYPE_F32};

	return setting_get(key, &v) == 0 ? v.f32 : 1.0f;
}

/* ---- FRAM ---- */

static int mount(bool format)
{
	static int last_rc;
	uint64_t v = 0;
	int rc;

	fs.flash_device = PARTITION_DEVICE(energy_partition);
	fs.offset = PARTITION_OFFSET(energy_partition);
	fs.sector_size = SECTOR_SIZE;
	fs.sector_count = PARTITION_SIZE(energy_partition) / SECTOR_SIZE;

	rc = format ? zms_mount_force(&fs) : zms_mount(&fs);
	if (rc == -ENOTSUP && !format) {
		/* Not a ZMS at all (a new board): the only case formatted on its own */
		LOG_WRN("no ZMS on the FRAM, formatting it");
		rc = zms_mount_force(&fs);
	}
	if (rc == 0) {
		rc = zms_read(&fs, ENERGY_ID, &v, sizeof(v));
		rc = (rc == sizeof(v) || rc == -ENOENT) ? 0 : (rc < 0 ? rc : -EIO);
	}
	if (rc != 0) {
		/* Retried every second; the stored count is left alone */
		if (rc != last_rc) {
			LOG_ERR("energy counter unavailable: %d", rc);
		}
		last_rc = rc;
		return rc;
	}
	energy_uwh = saved = v; /* 0 when never saved (-ENOENT) */
	mounted = true;
	LOG_INF("energy counter: %llu uWh", (unsigned long long)v);
	return 0;
}

static void tick_handler(struct k_work *work)
{
	if (!mounted) {
		(void)mount(false);
	}
	if (mounted && energy_uwh != saved) {
		uint64_t v = energy_uwh; /* on this stack, in SRAM */
		int rc = zms_write(&fs, ENERGY_ID, &v, sizeof(v));

		if (rc >= 0) {
			saved = v;
		} else {
			LOG_ERR("saving the energy counter: %d", rc);
		}
	}
	k_work_reschedule_for_queue(&wq, k_work_delayable_from_work(work), K_SECONDS(1));
}

static K_WORK_DELAYABLE_DEFINE(tick, tick_handler);

static void reset_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!mounted) {
		(void)mount(true);
	}
	energy_uwh = 0;
	energy_frac = 0.0;
	LOG_INF("energy counter reset");
}

static K_WORK_DEFINE(reset_work, reset_handler);

/* ---- Frames ---- */

static void frame_handler(struct k_work *work)
{
	uint8_t f[HLW8032_FRAME_LEN];
	struct energy_reading r;
	k_spinlock_key_t key;

	ARG_UNUSED(work);
	key = k_spin_lock(&lock);
	memcpy(f, frame, sizeof(f));
	r = reading;
	k_spin_unlock(&lock, key);

	hlw8032_parse(f, &regs);
	const uint16_t n = r.frames > 0 ? (uint16_t)(regs.pf - last_pf) : 0U;

	last_pf = regs.pf;
	if (n > 0U && mounted) {
		/* datasheet: 1 kWh = 1e9 * 3600 / (power parameter * VF * CF) pulses */
		const double uwh = energy_frac + n * (double)regs.p_par *
						  (double)(VF * CF * cal("energy/cal/energy")) / 3600.0;

		energy_uwh += (uint64_t)uwh;
		energy_frac = uwh - (double)(uint64_t)uwh;
	}
	r.pulses += n;

	const float kv = cal("energy/cal/voltage");
	const float ki = cal("energy/cal/current");

	r.voltage = regs.v ? (float)regs.v_par / regs.v * VF * kv : 0.0f;
	r.current = regs.i ? (float)regs.i_par / regs.i * CF * ki : 0.0f;
	r.power = regs.p ? (float)regs.p_par / regs.p * VF * CF * kv * ki : 0.0f;
	r.apparent_power = r.voltage * r.current;
	r.power_factor = r.apparent_power > 0.0f ? MIN(r.power / r.apparent_power, 1.0f) : 0.0f;
	r.energy_kwh = (double)energy_uwh / 1e9;
	r.frames++;

	key = k_spin_lock(&lock);
	reading = r;
	k_spin_unlock(&lock, key);
}

static K_WORK_DEFINE(frame_work, frame_handler);

/* In the UART interrupt; @p pause: the line went idle after these bytes */
static void rx_bytes(const uint8_t *p, size_t n, bool pause)
{
	while (n-- > 0U) {
		win[win_len++] = *p++;
		if (win_len == sizeof(win)) {
			if (hlw8032_frame_ok(win)) {
				k_spinlock_key_t key = k_spin_lock(&lock);

				memcpy(frame, win, sizeof(frame));
				k_spin_unlock(&lock, key);
				k_work_submit_to_queue(&wq, &frame_work);
			} else {
				dropped += sizeof(win);
			}
			win_len = 0;
		}
	}
	if (pause) {
		dropped += win_len; /* a frame cut short, or the tail of one at start */
		win_len = 0;
	}
}

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	static uint8_t next = 1;

	ARG_UNUSED(user_data);
	switch (evt->type) {
	case UART_RX_RDY:
		/* a buffer that is not full was reported on the idle line: a pause */
		rx_bytes(evt->data.rx.buf + evt->data.rx.offset, evt->data.rx.len,
			 evt->data.rx.offset + evt->data.rx.len < sizeof(rx_buf[0]));
		break;
	case UART_RX_BUF_REQUEST:
		uart_rx_buf_rsp(dev, rx_buf[next], sizeof(rx_buf[next]));
		next ^= 1U;
		break;
	case UART_RX_STOPPED: /* parity, framing or overrun error; RX_DISABLED follows */
		uart_errors++;
		break;
	case UART_RX_DISABLED:
		next = 1;
		uart_rx_enable(dev, rx_buf[0], sizeof(rx_buf[0]), 0);
		break;
	default:
		break;
	}
}

static void start_handler(struct k_work *work)
{
	int rc;

	ARG_UNUSED(work);
	(void)mount(false);
	rc = uart_callback_set(uart, uart_cb, NULL);
	if (rc == 0) {
		rc = uart_rx_enable(uart, rx_buf[0], sizeof(rx_buf[0]), 0);
	}
	if (rc != 0) {
		LOG_ERR("HLW8032 UART: %d", rc);
	}
	k_work_reschedule_for_queue(&wq, &tick, K_SECONDS(1));
}

static K_WORK_DEFINE(start_work, start_handler);

int energy_meter_start(void)
{
	if (!device_is_ready(uart)) {
		return -ENODEV;
	}
	k_work_queue_start(&wq, wq_stack, K_THREAD_STACK_SIZEOF(wq_stack), WQ_PRIO,
			   &(struct k_work_queue_config){.name = "energy_meter"});
	k_work_submit_to_queue(&wq, &start_work);
	return 0;
}

void energy_meter_get(struct energy_reading *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = reading;
	k_spin_unlock(&lock, key);
}

/* ---- Shell ---- */

#if defined(CONFIG_SHELL)
/* No float printf in this build: thousandths */
static uint32_t milli(float x)
{
	return (uint32_t)(x * 1000.0f + 0.5f);
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	struct energy_reading r;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	energy_meter_get(&r);

	const uint64_t e = (uint64_t)(r.energy_kwh * 1e6 + 0.5); /* 1e-6 kWh */

	shell_print(sh, "voltage   %u.%03u V", milli(r.voltage) / 1000U, milli(r.voltage) % 1000U);
	shell_print(sh, "current   %u.%03u A", milli(r.current) / 1000U, milli(r.current) % 1000U);
	shell_print(sh, "power     %u.%03u W", milli(r.power) / 1000U, milli(r.power) % 1000U);
	shell_print(sh, "apparent  %u.%03u VA", milli(r.apparent_power) / 1000U,
		    milli(r.apparent_power) % 1000U);
	shell_print(sh, "pf        %u.%03u", milli(r.power_factor) / 1000U,
		    milli(r.power_factor) % 1000U);
	shell_print(sh, "energy    %llu.%06llu kWh (%llu pulses)", (unsigned long long)(e / 1000000U),
		    (unsigned long long)(e % 1000000U), (unsigned long long)r.pulses);
	shell_print(sh, "frames %u, bytes dropped %u, uart errors %u, counter %s", r.frames,
		    dropped, uart_errors, mounted ? "mounted" : "NOT mounted");
	shell_print(sh, "cal voltage %u, current %u, energy %u (x1000)",
		    milli(cal("energy/cal/voltage")), milli(cal("energy/cal/current")),
		    milli(cal("energy/cal/energy")));
	return 0;
}

static int cmd_cal(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const keys[] = {"energy/cal/voltage", "energy/cal/current",
					   "energy/cal/energy"};
	const char *which = strchr("vie", argv[1][0]);
	struct setting_value v = {.type = SETTING_TYPE_F32, .f32 = strtof(argv[2], NULL)};
	int rc;

	ARG_UNUSED(argc);
	if (which == NULL || argv[1][1] != '\0') {
		shell_error(sh, "which: v, i or e");
		return -EINVAL;
	}
	rc = setting_set(keys[which - "vie"], &v);
	if (rc != 0) {
		shell_error(sh, "%s: %d (range 0.5..2.0)", keys[which - "vie"], rc);
	}
	return rc;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_work_submit_to_queue(&wq, &reset_work);
	shell_print(sh, "energy counter reset requested");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(energy_cmds,
	SHELL_CMD_ARG(cal, NULL, "Set a coefficient: cal <v|i|e> <0.5..2.0>", cmd_cal, 3, 0),
	SHELL_CMD(reset, NULL, "Zero the energy counter", cmd_reset),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(energy, &energy_cmds, "HLW8032 values and the energy counter", cmd_show);
#endif
