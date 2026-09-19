/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Debug shell: sample VDDA, which on this board is VDD (the LDO output), through
 * VREFINT and its factory calibration, to see whether the resets are supply dips
 * (owner's request 2026-09-19, reports/vdd).
 *
 *   vdd [count] [period_us]    default 1000 samples, 10000 us apart
 *
 * The samples are taken by the vdd_monitor thread, which sleeps period_us after
 * each one (0: only k_yield()), so the rest of the firmware keeps running and the
 * window is about count * period_us: 10 s by default. The shell waits for the
 * result and prints min, max, mean and median in mV, the real window, the longest
 * gap between two samples (a cooperative thread holding the CPU shows up here) and
 * when the minimum was seen; the thread logs the same line (LOG_INF), which the log
 * export keeps when the shell output is lost.
 *
 * One conversion goes through Zephyr's stm32_vref driver: VREFINT path on, 5 us
 * settling (one tick here), the longest sampling time, path off. Its result is
 * VREF+, which follows VDDA; resolution ~1 mV.
 */

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(vdd_monitor, LOG_LEVEL_INF);

#if defined(CONFIG_SHELL) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(vref1))

#define VDD_MAX_SAMPLES     4000U
#define VDD_DEFAULT_SAMPLES 1000U
#define VDD_DEFAULT_PERIOD  10000U

struct vdd_result {
	int rc;
	uint32_t count;
	uint32_t failed;
	uint16_t min_mv;
	uint16_t max_mv;
	uint32_t median_x10;
	uint32_t mean_x10;
	uint32_t window_ms;
	uint32_t max_gap_us;
	uint32_t min_at_ms;
};

static const struct device *const vref = DEVICE_DT_GET(DT_NODELABEL(vref1));

static uint16_t samples[VDD_MAX_SAMPLES];
static uint32_t req_count;
static uint32_t req_period_us;
static struct vdd_result result;

static K_SEM_DEFINE(req_sem, 0, 1);
static K_SEM_DEFINE(done_sem, 0, 1);
static K_MUTEX_DEFINE(busy);

static int cmp_u16(const void *a, const void *b)
{
	return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

static int sample_mv(uint16_t *mv)
{
	struct sensor_value val;
	int rc = sensor_sample_fetch(vref);

	if (rc == 0) {
		rc = sensor_channel_get(vref, SENSOR_CHAN_VOLTAGE, &val);
	}
	if (rc == 0) {
		*mv = (uint16_t)sensor_value_to_milli(&val);
	}
	return rc;
}

static void measure(uint32_t count, uint32_t period_us, struct vdd_result *r)
{
	const int64_t start = k_uptime_ticks();
	int64_t prev = start;
	uint64_t sum = 0U;
	uint32_t n = 0U;

	*r = (struct vdd_result){.min_mv = UINT16_MAX};
	for (uint32_t i = 0U; i < count; i++) {
		const int64_t now = k_uptime_ticks();
		const uint32_t gap = (uint32_t)k_ticks_to_us_floor64(now - prev);
		uint16_t mv;

		prev = now;
		if (n > 0U && gap > r->max_gap_us) {
			r->max_gap_us = gap;
		}
		if (sample_mv(&mv) != 0) {
			r->failed++;
		} else {
			samples[n++] = mv;
			sum += mv;
			if (mv < r->min_mv) {
				r->min_mv = mv;
				r->min_at_ms = (uint32_t)k_ticks_to_ms_floor64(now - start);
			}
			if (mv > r->max_mv) {
				r->max_mv = mv;
			}
		}
		if (period_us > 0U) {
			k_sleep(K_USEC(period_us));
		} else {
			k_yield();
		}
	}
	r->window_ms = (uint32_t)k_ticks_to_ms_floor64(k_uptime_ticks() - start);
	r->count = n;
	if (n == 0U) {
		r->rc = -EIO;
		return;
	}
	r->mean_x10 = (uint32_t)((sum * 10U + n / 2U) / n);
	qsort(samples, n, sizeof(samples[0]), cmp_u16);
	r->median_x10 = (n % 2U) ? samples[n / 2U] * 10U
				 : (samples[n / 2U - 1U] + samples[n / 2U]) * 5U;
}

static void vdd_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&req_sem, K_FOREVER);
		measure(req_count, req_period_us, &result);
		/* Also into the log, from here and not from the shell: a telnet session can
		 * lose the shell output under heavy HTTP traffic (a measurement across an
		 * upload, 2026-09-19); GET /api/v1/logs/export keeps it. */
		if (result.rc == 0) {
			LOG_INF("VDDA mV min %u max %u mean %u.%u median %u.%u; %u samples, %u ms, "
				"gap %u us, min at %u ms",
				result.min_mv, result.max_mv, result.mean_x10 / 10U,
				result.mean_x10 % 10U, result.median_x10 / 10U,
				result.median_x10 % 10U, result.count, result.window_ms,
				result.max_gap_us, result.min_at_ms);
		}
		k_sem_give(&done_sem);
	}
}

K_THREAD_DEFINE(vdd_monitor, 2048, vdd_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(3), 0, 0);

static int cmd_vdd(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t count = VDD_DEFAULT_SAMPLES;
	uint32_t period_us = VDD_DEFAULT_PERIOD;
	k_timeout_t wait;

	if (argc > 1) {
		count = strtoul(argv[1], NULL, 0);
	}
	if (argc > 2) {
		period_us = strtoul(argv[2], NULL, 0);
	}
	if (count == 0U || count > VDD_MAX_SAMPLES) {
		shell_error(sh, "count 1..%u", VDD_MAX_SAMPLES);
		return -EINVAL;
	}
	if (!device_is_ready(vref)) {
		shell_error(sh, "vref sensor not ready");
		return -ENODEV;
	}
	if (k_mutex_lock(&busy, K_NO_WAIT) != 0) {
		shell_error(sh, "a measurement is already running");
		return -EBUSY;
	}

	shell_print(sh, "sampling %u x VDDA, %u us apart...", count, period_us);
	req_count = count;
	req_period_us = period_us;
	k_sem_reset(&done_sem);
	k_sem_give(&req_sem);
	/* each sample also sleeps a tick or two in the driver */
	wait = K_MSEC((uint64_t)count * (period_us / 1000U + 2U) + 5000U);
	if (k_sem_take(&done_sem, wait) != 0) {
		/* the thread still owns the buffer: keep busy locked until it is done */
		k_sem_take(&done_sem, K_FOREVER);
		k_mutex_unlock(&busy);
		shell_error(sh, "timed out");
		return -ETIMEDOUT;
	}
	k_mutex_unlock(&busy);

	if (result.rc != 0) {
		shell_error(sh, "no sample read (%u failed)", result.failed);
		return result.rc;
	}
	shell_print(sh, "VDDA mV: min %u max %u mean %u.%u median %u.%u", result.min_mv,
		    result.max_mv, result.mean_x10 / 10U, result.mean_x10 % 10U,
		    result.median_x10 / 10U, result.median_x10 % 10U);
	shell_print(sh, "samples %u (failed %u), window %u ms, longest gap %u us, min at %u ms",
		    result.count, result.failed, result.window_ms, result.max_gap_us,
		    result.min_at_ms);
	return 0;
}

SHELL_CMD_ARG_REGISTER(vdd, NULL,
		       "Sample VDDA through VREFINT: vdd [count<=4000] [period_us], "
		       "default 1000 x 10000 us",
		       cmd_vdd, 1, 2);

#endif /* CONFIG_SHELL && vref1 okay */
