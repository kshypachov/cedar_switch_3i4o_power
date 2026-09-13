/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See network_service.h.
 *
 * One worker, preemptive and below the HTTP server, runs
 * network_manager_process(): when a request wakes it and once a second, which
 * is what advances the confirmation deadline. Everything that blocks on the
 * network — joining Wi-Fi, a scan of up to ten seconds, the commit written to
 * settings — happens there and nowhere else.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <device_config_store/device_config_store.h>
#include <network_manager/network_manager.h>
#include <settings_registry/settings_registry.h>

#include "boot_streak.h"
#include "net_adapter.h"
#include "netcfg_store.h"
#include "network_service.h"

LOG_MODULE_REGISTER(network_service, LOG_LEVEL_INF);

#define WORKER_STACK_SIZE 8192
/* Below the cooperative HTTP server, above nothing that matters to a user. */
#define WORKER_PRIORITY   K_PRIO_PREEMPT(10)
/* One pass may leave work for the next (a rollback after a failed push). */
#define PASSES_PER_RUN    4

#define BOOT_STREAK_KEY "net/boot_streak"

static const uint32_t streak_default;
static uint32_t streak_storage;

SETTING_REGISTRY_DEFINE(network_boot_streak, .key = BOOT_STREAK_KEY, .type = SETTING_TYPE_U32,
			.storage = SETTING_STORAGE_OWNED, .persistence = SETTING_PERSISTENT,
			.mirror = SETTING_MIRROR_RAM, .default_value = &streak_default,
			.ram_storage = &streak_storage);

K_THREAD_STACK_DEFINE(network_worker_stack, WORKER_STACK_SIZE);

static struct k_work_q worker;
static struct k_work start_work;
static struct k_work run_work;
static struct k_work_delayable tick_work;
static struct k_work_delayable streak_work;
static struct device_config_backend backend;
static K_SEM_DEFINE(start_done, 0, 1);
static int start_rc;
static bool started;

static void run(struct k_work *work)
{
	ARG_UNUSED(work);

	net_adapter_refresh();
	for (int i = 0; i < PASSES_PER_RUN; i++) {
		if (network_manager_process() == 0) {
			break;
		}
	}
}

static void tick(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)k_work_submit_to_queue(&worker, &run_work);
	(void)k_work_reschedule_for_queue(&worker, &tick_work, K_SECONDS(1));
}

static int store_streak(uint32_t value)
{
	const struct setting_value v = {.type = SETTING_TYPE_U32, .u32 = value};

	return setting_set(BOOT_STREAK_KEY, &v);
}

/* The boot stayed up long enough: it no longer counts towards a recovery. */
static void clear_streak(struct k_work *work)
{
	ARG_UNUSED(work);

	if (store_streak(0) != 0) {
		LOG_WRN("clearing the boot count failed");
	}
}

void network_service_kick(void)
{
	if (started) {
		(void)k_work_submit_to_queue(&worker, &run_work);
	}
}

uint32_t network_service_wifi_security_modes(void)
{
	/*
	 * What this build's Wi-Fi driver joins (esp_hosted_mcu_connect: none,
	 * a PSK, an SAE password). Whether the coprocessor is there to do it is
	 * reported by GET /network/status, not by leaving modes out: the
	 * contract requires at least one.
	 */
	return BIT(DEVICE_CONFIG_WIFI_OPEN) | BIT(DEVICE_CONFIG_WIFI_WPA2_PSK) |
	       BIT(DEVICE_CONFIG_WIFI_WPA3_SAE);
}

static void count_this_boot(void)
{
	struct setting_value stored = {.type = SETTING_TYPE_U32};
	uint32_t next;

	if (setting_get(BOOT_STREAK_KEY, &stored) != 0) {
		stored.u32 = 0;
	}
	const bool recover = boot_streak_step(stored.u32, BOOT_STREAK_THRESHOLD, &next);

	/* Stored first: a power loss during the restore restores again next boot. */
	if (store_streak(next) != 0) {
		LOG_WRN("storing the boot count failed");
	}
	LOG_INF("boot %u of %u in a row shorter than %d s", next, BOOT_STREAK_THRESHOLD,
		BOOT_STREAK_WINDOW_SECONDS);
	if (!recover) {
		return;
	}

	LOG_WRN("%u short boots in a row: restoring the factory network configuration",
		BOOT_STREAK_THRESHOLD);
	if (network_manager_restore_defaults() == 0) {
		(void)store_streak(0);
	}
}

static const char *recovery_str(enum device_config_recovery result)
{
	switch (result) {
	case DEVICE_CONFIG_RECOVERY_CLEAN:
		return "clean";
	case DEVICE_CONFIG_RECOVERY_DEFAULTS:
		return "no stored configuration, factory defaults";
	case DEVICE_CONFIG_RECOVERY_ROLLED_BACK:
		return "an unconfirmed change was rolled back";
	case DEVICE_CONFIG_RECOVERY_COMMIT_COMPLETED:
		return "an interrupted commit was completed";
	case DEVICE_CONFIG_RECOVERY_PENDING_CORRUPT:
		return "an unreadable pending change was discarded";
	case DEVICE_CONFIG_RECOVERY_COMMITTED_LOST:
		return "the stored configuration was unreadable, factory defaults";
	case DEVICE_CONFIG_RECOVERY_COMMITTED_DEGRADED:
		return "an older copy of the configuration was used";
	default:
		return "unknown";
	}
}

/*
 * The start sequence is the worker's first job, not main()'s: loading the
 * store holds two 512-byte slot records and device-config-store's own records
 * on the stack at once, and main's 2 KiB stack overflowed on board B
 * (docs/device-development/reports/p4/hw).
 */
static void start(struct k_work *work)
{
	ARG_UNUSED(work);

	struct device_config_recovery_report report = {0};
	int64_t started_at = k_uptime_get();
	int rc;

	netcfg_store_bind(&netcfg_registry_kv, &backend);
	rc = device_config_init(&backend, &report);
	if (rc != 0) {
		LOG_ERR("network configuration store failed (%d)", rc);
	} else {
		LOG_INF("network configuration revision %u: %s (%lld ms)", report.revision,
			recovery_str(report.result), k_uptime_get() - started_at);
	}

	rc = net_adapter_init(network_service_kick);
	if (rc != 0) {
		LOG_ERR("network adapter: %d", rc);
	}
	rc = network_manager_init(&net_adapter_ops);
	if (rc != 0) {
		LOG_ERR("network manager: %d", rc);
		start_rc = rc;
		k_sem_give(&start_done);
		return;
	}

	count_this_boot();
	network_manager_boot(&report);

	started = true;
	(void)k_work_submit_to_queue(&worker, &run_work);
	(void)k_work_reschedule_for_queue(&worker, &tick_work, K_SECONDS(1));
	(void)k_work_reschedule_for_queue(&worker, &streak_work,
					  K_SECONDS(BOOT_STREAK_WINDOW_SECONDS));

#ifdef CONFIG_THREAD_STACK_INFO
	size_t unused;

	if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
		LOG_INF("worker stack: %zu of %u bytes never used", unused, WORKER_STACK_SIZE);
	}
#endif
	start_rc = 0;
	k_sem_give(&start_done);
}

int network_service_start(void)
{
	k_work_queue_init(&worker);
	k_work_queue_start(&worker, network_worker_stack,
			   K_THREAD_STACK_SIZEOF(network_worker_stack), WORKER_PRIORITY,
			   &(struct k_work_queue_config){.name = "network"});
	k_work_init(&start_work, start);
	k_work_init(&run_work, run);
	k_work_init_delayable(&tick_work, tick);
	k_work_init_delayable(&streak_work, clear_streak);

	/* Returns once network-manager is ready for the web API that starts next. */
	(void)k_work_submit_to_queue(&worker, &start_work);
	(void)k_sem_take(&start_done, K_FOREVER);

	return start_rc;
}
