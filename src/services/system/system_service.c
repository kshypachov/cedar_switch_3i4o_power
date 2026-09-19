/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board's system service: reset cause, independent watchdog, the platforms of
 * system-image-store and system-updater, and the `sysupd` shell commands
 * (system_service.h).
 *
 * MCUboot is reached through bootutil_public.c (CONFIG_MCUBOOT_BOOTUTIL_LIB), not
 * zephyr's subsys/dfu/boot/mcuboot.c: that file takes the active slot from the
 * chosen zephyr,code-partition, which on this board is the memory region
 * ext_flash_mem rather than a partition, and does not compile. bootutil opens
 * slot0_partition and slot1_partition by label.
 */

#include "system_service.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <bootutil/bootutil_public.h>
#include <bootutil/image.h>

#if defined(CONFIG_SYSTEM_IMAGE_STORE) && defined(CONFIG_SYSTEM_UPDATER)
#include <system_image_store/system_image_store.h>
#include <system_updater/system_updater.h>
#define HAVE_UPDATE 1
#else
#define HAVE_UPDATE 0
#endif
#if defined(CONFIG_FIRMWARE_STORE)
#include <firmware_store/firmware_store.h>
#endif

LOG_MODULE_REGISTER(system_service, LOG_LEVEL_INF);

#define SLOT0_ID PARTITION_ID(slot0_partition)
#define SLOT1_ID PARTITION_ID(slot1_partition)

/* MCUboot's view of slot 2 has 64 KiB sectors (sysbuild/mcuboot.conf); its
 * trailer lives in the last one. */
#define TRAILER_SECTOR_BYTES (64U * 1024U)
#define SECTOR_BYTES         4096U

#define FIRMWARE_DIR  "/lfs/firmware"
#define JOURNAL_PATH  FIRMWARE_DIR "/sysupd.journal"
#define JOURNAL_TMP   FIRMWARE_DIR "/sysupd.journal.tmp"
#define JOURNAL_MAGIC 0x53555044U /* "SUPD" */

/* -- reset cause ------------------------------------------------------------- */

static uint32_t reset_cause;

static int capture_reset_cause(void)
{
	if (hwinfo_get_reset_cause(&reset_cause) != 0) {
		reset_cause = 0;
	}
	return 0;
}

SYS_INIT(capture_reset_cause, PRE_KERNEL_1, 0);

uint32_t system_service_reset_cause(void)
{
	return reset_cause;
}

static void print_reset_cause(const struct shell *sh, uint32_t cause)
{
	static const struct {
		uint32_t flag;
		const char *name;
	} names[] = {
		{RESET_PIN, "pin"},
		{RESET_SOFTWARE, "software"},
		{RESET_BROWNOUT, "brownout"},
		{RESET_POR, "power-on"},
		{RESET_WATCHDOG, "watchdog"},
		{RESET_DEBUG, "debug"},
		{RESET_SECURITY, "security"},
		{RESET_LOW_POWER_WAKE, "low-power"},
		{RESET_CPU_LOCKUP, "lockup"},
		{RESET_PARITY, "parity"},
		{RESET_PLL, "pll"},
		{RESET_CLOCK, "clock"},
		{RESET_HARDWARE, "hardware"},
		{RESET_USER, "user"},
		{RESET_TEMPERATURE, "temperature"},
	};

	shell_fprintf(sh, SHELL_NORMAL, "reset cause 0x%08x:", cause);
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if ((cause & names[i].flag) != 0U) {
			shell_fprintf(sh, SHELL_NORMAL, " %s", names[i].name);
		}
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
}

/* -- independent watchdog ------------------------------------------------------ */

#if defined(CONFIG_APP_IWDG) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(iwdg))

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(iwdg));
static atomic_t wdt_running;
static atomic_t wdt_feeds;

static void wdt_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	const struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = CONFIG_APP_IWDG_TIMEOUT_MS},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int channel;
	int rc;

	if (!device_is_ready(wdt)) {
		LOG_ERR("IWDG not ready: the board runs without a watchdog");
		return;
	}
	channel = wdt_install_timeout(wdt, &cfg);
	if (channel < 0) {
		LOG_ERR("IWDG timeout %u ms refused: %d", CONFIG_APP_IWDG_TIMEOUT_MS, channel);
		return;
	}
	rc = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (rc != 0) {
		LOG_ERR("IWDG setup failed: %d", rc);
		return;
	}
	atomic_set(&wdt_running, 1);
	LOG_INF("IWDG running, timeout %u ms", CONFIG_APP_IWDG_TIMEOUT_MS);

	for (;;) {
		(void)wdt_feed(wdt, channel);
		atomic_inc(&wdt_feeds);
		k_msleep(CONFIG_APP_IWDG_TIMEOUT_MS / 4U);
	}
}

K_THREAD_DEFINE(app_iwdg, 1024, wdt_thread, NULL, NULL, NULL, K_PRIO_COOP(0), 0, 0);

#endif /* CONFIG_APP_IWDG */

/* -- slots ---------------------------------------------------------------------- */

/* In SRAM with the rest of this library: see CMakeLists.txt. */
static uint8_t copy_buf[SECTOR_BYTES];

struct image_info {
	bool valid;
	uint8_t major;
	uint8_t minor;
	uint16_t revision;
	uint32_t build;
	/** Header + body + protected TLV + TLV: the length of zephyr.signed.bin. */
	uint32_t total;
	bool has_hash;
	uint8_t hash[32];
};

static int read_image(int area_id, struct image_info *out)
{
	const struct flash_area *fa;
	struct image_header hdr;
	struct image_tlv_info info;
	struct image_tlv tlv;
	uint32_t off;
	uint32_t end;
	int rc;

	memset(out, 0, sizeof(*out));
	rc = flash_area_open(area_id, &fa);
	if (rc != 0) {
		return rc;
	}
	rc = flash_area_read(fa, 0, &hdr, sizeof(hdr));
	if (rc != 0 || sys_le32_to_cpu(hdr.ih_magic) != IMAGE_MAGIC) {
		goto done;
	}
	out->major = hdr.ih_ver.iv_major;
	out->minor = hdr.ih_ver.iv_minor;
	out->revision = sys_le16_to_cpu(hdr.ih_ver.iv_revision);
	out->build = sys_le32_to_cpu(hdr.ih_ver.iv_build_num);

	off = (uint32_t)sys_le16_to_cpu(hdr.ih_hdr_size) + sys_le32_to_cpu(hdr.ih_img_size) +
	      sys_le16_to_cpu(hdr.ih_protect_tlv_size);
	if (off + sizeof(info) > fa->fa_size) {
		goto done;
	}
	rc = flash_area_read(fa, off, &info, sizeof(info));
	if (rc != 0 || sys_le16_to_cpu(info.it_magic) != IMAGE_TLV_INFO_MAGIC) {
		goto done;
	}
	end = off + sys_le16_to_cpu(info.it_tlv_tot);
	if (end > fa->fa_size) {
		goto done;
	}
	out->total = end;
	out->valid = true;

	for (off += sizeof(info); off + sizeof(tlv) <= end;) {
		uint16_t type;
		uint16_t len;

		rc = flash_area_read(fa, off, &tlv, sizeof(tlv));
		if (rc != 0) {
			break;
		}
		type = sys_le16_to_cpu(tlv.it_type);
		len = sys_le16_to_cpu(tlv.it_len);
		if (type == IMAGE_TLV_SHA256 && len == sizeof(out->hash)) {
			out->has_hash = flash_area_read(fa, off + sizeof(tlv), out->hash,
							sizeof(out->hash)) == 0;
			break;
		}
		off += sizeof(tlv) + len;
	}
	rc = 0;
done:
	flash_area_close(fa);
	return rc;
}

/*
 * As zephyr's boot_is_img_confirmed(): an image whose trailer magic is unset was
 * programmed directly (debugger, recovery) and MCUboot boots it as confirmed.
 */
static int running_confirmed(bool *confirmed)
{
	struct boot_swap_state st;
	int rc = boot_read_swap_state_by_id(SLOT0_ID, &st);

	if (rc != 0) {
		return rc;
	}
	*confirmed = st.magic != BOOT_MAGIC_GOOD || st.image_ok == BOOT_FLAG_SET;
	return 0;
}

static bool swap_requested(void)
{
	const int type = boot_swap_type();

	return type == BOOT_SWAP_TYPE_TEST || type == BOOT_SWAP_TYPE_PERM;
}

static const char *confirmed_str(void)
{
	bool confirmed;

	if (running_confirmed(&confirmed) != 0) {
		return "unknown";
	}
	return confirmed ? "confirmed" : "NOT confirmed";
}

/*
 * The XIP-safe write bounces a source buffer only when it lies outside internal
 * SRAM, into a buffer on the caller's stack, and reads it with OCTOSPI1 out of
 * memory-mapped mode. bootutil's trailer write keeps its buffer on the stack too.
 * A stack in PSRAM (0x7xxxxxxx, the v1 worker's) must not get there.
 */
static bool stack_in_sram(void)
{
	const int probe = 0;

	return ((uintptr_t)&probe >> 28) == 0x2U;
}

static int confirm_running(void)
{
	if (!stack_in_sram()) {
		LOG_ERR("confirmation refused on a stack outside SRAM (thread %p)",
			(void *)k_current_get());
		return -EPERM;
	}
	return boot_set_confirmed() == 0 ? 0 : -EIO;
}

/* -- the update modules' platforms ------------------------------------------- */

#if HAVE_UPDATE

static const struct flash_area *slot2;
static bool update_ready;
static uint32_t upload_max_bytes;

static int slot_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
{
	ARG_UNUSED(ctx);
	return flash_area_read(slot2, offset, buf, len);
}

static int slot_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
	ARG_UNUSED(ctx);
	return flash_area_write(slot2, offset, data, len);
}

static int slot_erase(void *ctx, uint32_t offset, uint32_t len)
{
	ARG_UNUSED(ctx);
	return flash_area_erase(slot2, offset, len);
}

static bool slot_locked(void *ctx)
{
	bool confirmed;

	ARG_UNUSED(ctx);
	if (running_confirmed(&confirmed) != 0) {
		return true;
	}
	return !confirmed || swap_requested();
}

/* Filled at start: the slot's size comes from the flash area. */
static struct sys_img_platform store_platform = {
	.read = slot_read,
	.write = slot_write,
	.erase = slot_erase,
	.slot_locked = slot_locked,
	.sector_size = SECTOR_BYTES,
	.trailer_bytes = TRAILER_SECTOR_BYTES,
	.image = {
		/* MCUboot jumps to slot base + header size (imgtool --header-size). */
		.header_size = CONFIG_ROM_START_OFFSET,
		.ram = {
			/* The initial SP of this application is in SRAM (kernel stacks). */
			{DT_REG_ADDR(DT_CHOSEN(zephyr_sram)),
			 DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) + DT_REG_SIZE(DT_CHOSEN(zephyr_sram))},
			/* PSRAM, memory-mapped by MCUboot before the jump (psram_selftest()). */
			{0x70000000U, 0x70800000U},
		},
		/* The application is linked at the ICACHE alias of the OSPI NOR. */
		.exec = {DT_REG_ADDR(DT_NODELABEL(ext_flash_mem)) + CONFIG_ROM_START_OFFSET,
			 DT_REG_ADDR(DT_NODELABEL(ext_flash_mem)) +
				 DT_REG_SIZE(DT_NODELABEL(ext_flash_mem))},
	},
};

static int up_running_image(void *ctx, struct system_image *out, bool *confirmed)
{
	struct image_info img;

	ARG_UNUSED(ctx);
	if (read_image(SLOT0_ID, &img) != 0 || !img.valid || !img.has_hash) {
		return -EIO;
	}
	out->version.major = img.major;
	out->version.minor = img.minor;
	out->version.revision = img.revision;
	out->version.build = img.build;
	memcpy(out->hash, img.hash, sizeof(out->hash));
	return running_confirmed(confirmed) == 0 ? 0 : -EIO;
}

static int up_staged_image(void *ctx, const char *upload_id, struct system_image *out,
			   uint32_t *size)
{
	struct sys_img_image img;
	int rc;

	ARG_UNUSED(ctx);
	rc = sys_img_image_info(upload_id, &img);
	if (rc != 0) {
		return rc;
	}
	out->version.major = img.major;
	out->version.minor = img.minor;
	out->version.revision = img.revision;
	out->version.build = img.build;
	memcpy(out->hash, img.image_hash, sizeof(out->hash));
	*size = img.size_bytes;
	return 0;
}

static int up_set_upload_in_use(void *ctx, const char *upload_id, bool in_use)
{
	ARG_UNUSED(ctx);
	return sys_img_set_in_use(upload_id, in_use);
}

/* Called from system_updater_init() on main, before the v1 worker runs: the one
 * moment sys_img_delete() may run outside the worker. */
static void up_upload_consumed(void *ctx, const char *upload_id)
{
	int rc;

	ARG_UNUSED(ctx);
	rc = sys_img_delete(upload_id);
	if (rc != 0 && rc != -ENOENT) {
		LOG_WRN("upload %s consumed by the swap but not forgotten: %d", upload_id, rc);
	}
}

static int up_request_swap(void *ctx)
{
	ARG_UNUSED(ctx);
	return boot_set_pending(0) == 0 ? 0 : -EIO;
}

static bool up_swap_pending(void *ctx)
{
	ARG_UNUSED(ctx);
	return swap_requested();
}

static int up_confirm(void *ctx)
{
	ARG_UNUSED(ctx);
	return confirm_running();
}

static void up_reboot(void *ctx)
{
	ARG_UNUSED(ctx);
	LOG_WRN("restarting for the MCUboot swap");
	log_panic();
	sys_reboot(SYS_REBOOT_WARM);
}

/* The journal: magic, payload size, payload, CRC32 of the payload. */
struct journal_file {
	uint32_t magic;
	uint32_t size;
	struct system_update_journal journal;
	uint32_t crc;
} __packed;

static K_MUTEX_DEFINE(journal_lock);
static struct journal_file journal_buf;

static int up_journal_load(void *ctx, struct system_update_journal *out)
{
	struct fs_file_t f;
	ssize_t n;
	int rc;

	ARG_UNUSED(ctx);
	k_mutex_lock(&journal_lock, K_FOREVER);
	fs_file_t_init(&f);
	rc = fs_open(&f, JOURNAL_PATH, FS_O_READ);
	if (rc != 0) {
		rc = rc == -ENOENT ? -ENOENT : -EIO;
		goto out;
	}
	n = fs_read(&f, &journal_buf, sizeof(journal_buf));
	(void)fs_close(&f);
	if (n != (ssize_t)sizeof(journal_buf) || journal_buf.magic != JOURNAL_MAGIC ||
	    journal_buf.size != sizeof(journal_buf.journal) ||
	    journal_buf.crc != crc32_ieee((const uint8_t *)&journal_buf.journal,
					  sizeof(journal_buf.journal))) {
		LOG_WRN("update journal unreadable (%d bytes), treated as none", (int)n);
		rc = -EIO;
		goto out;
	}
	*out = journal_buf.journal;
	rc = 0;
out:
	k_mutex_unlock(&journal_lock);
	return rc;
}

static int up_journal_save(void *ctx, const struct system_update_journal *journal)
{
	struct fs_file_t f;
	ssize_t n;
	int rc;

	ARG_UNUSED(ctx);
	k_mutex_lock(&journal_lock, K_FOREVER);
	journal_buf.magic = JOURNAL_MAGIC;
	journal_buf.size = sizeof(journal_buf.journal);
	journal_buf.journal = *journal;
	journal_buf.crc =
		crc32_ieee((const uint8_t *)&journal_buf.journal, sizeof(journal_buf.journal));

	fs_file_t_init(&f);
	rc = fs_open(&f, JOURNAL_TMP, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		goto out;
	}
	n = fs_write(&f, &journal_buf, sizeof(journal_buf));
	rc = n == (ssize_t)sizeof(journal_buf) ? fs_sync(&f) : -EIO;
	(void)fs_close(&f);
	if (rc == 0) {
		rc = fs_rename(JOURNAL_TMP, JOURNAL_PATH);
	}
out:
	k_mutex_unlock(&journal_lock);
	if (rc != 0) {
		LOG_ERR("update journal not saved: %d", rc);
		return -EIO;
	}
	return 0;
}

static int64_t up_now_ms(void *ctx)
{
	ARG_UNUSED(ctx);
	return k_uptime_get();
}

static void up_sleep_ms(void *ctx, uint32_t ms)
{
	ARG_UNUSED(ctx);
	k_msleep(ms);
}

static const struct system_updater_platform updater_platform = {
	.running_image = up_running_image,
	.staged_image = up_staged_image,
	.set_upload_in_use = up_set_upload_in_use,
	.upload_consumed = up_upload_consumed,
	.request_swap = up_request_swap,
	.swap_pending = up_swap_pending,
	.confirm = up_confirm,
	.reboot = up_reboot,
	.journal_load = up_journal_load,
	.journal_save = up_journal_save,
	.now_ms = up_now_ms,
	.sleep_ms = up_sleep_ms,
	.ctx = NULL,
};

/*
 * Self-confirmation and expiry. Its own thread rather than a work queue, for its
 * stack: in this library, hence in SRAM, which the confirmation's XIP write needs.
 */
static void update_thread(void *a, void *b, void *c)
{
	uint32_t seconds = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_sleep(K_SECONDS(1));
		if (!update_ready) {
			continue;
		}
		const int64_t now = k_uptime_get();

		system_updater_tick(now);
		if (++seconds % 60U == 0U) {
			sys_img_tick(now);
		}
	}
}

K_THREAD_DEFINE(sysupd_tick, 4096, update_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, 0);

static void update_start(void)
{
	int rc = flash_area_open(SLOT1_ID, &slot2);

	if (rc != 0) {
		LOG_ERR("slot 2 unavailable: %d", rc);
		return;
	}
	store_platform.slot_size = slot2->fa_size;
#if defined(CONFIG_SYSTEM_IMAGE_STORE_STREAM_FLASH)
	/* Sector-aligned chunks go through stream_flash on the SPI NOR (prj.conf). */
	store_platform.flash_dev = flash_area_get_device(slot2);
	store_platform.flash_offset = (uint32_t)slot2->fa_off;
#endif
	upload_max_bytes = slot2->fa_size - TRAILER_SECTOR_BYTES;

	rc = sys_img_init(FIRMWARE_DIR, &store_platform, k_uptime_get());
	if (rc != 0) {
		LOG_ERR("system-image-store: %d; STM32 update unavailable", rc);
		return;
	}
	rc = system_updater_init(&updater_platform);
	if (rc != 0) {
		LOG_ERR("system-updater: %d; STM32 update unavailable", rc);
		return;
	}
	update_ready = true;
}

#else /* !HAVE_UPDATE */

static const bool update_ready;
static const uint32_t upload_max_bytes;

static void update_start(void)
{
}

#endif /* HAVE_UPDATE */

bool system_service_update_ready(void)
{
	return update_ready;
}

uint32_t system_service_upload_max_bytes(void)
{
	return upload_max_bytes;
}

/* -- shell ---------------------------------------------------------------------- */

static void print_image(const struct shell *sh, const char *name, const struct image_info *img)
{
	if (!img->valid) {
		shell_print(sh, "%s: no image", name);
		return;
	}
	shell_fprintf(sh, SHELL_NORMAL, "%s: %u.%u.%u+%u, %u bytes, hash ", name, img->major,
		      img->minor, img->revision, img->build, img->total);
	if (img->has_hash) {
		for (size_t i = 0; i < 8; i++) {
			shell_fprintf(sh, SHELL_NORMAL, "%02x", img->hash[i]);
		}
		shell_fprintf(sh, SHELL_NORMAL, "...\n");
	} else {
		shell_fprintf(sh, SHELL_NORMAL, "none\n");
	}
}

static void print_swap_state(const struct shell *sh, const char *name, int area_id)
{
	struct boot_swap_state st;
	int rc = boot_read_swap_state_by_id(area_id, &st);

	if (rc != 0) {
		shell_print(sh, "%s trailer: read failed %d", name, rc);
		return;
	}
	/* BOOT_MAGIC_GOOD 1, BAD 2, UNSET 3; BOOT_FLAG_SET 1, UNSET 3 */
	shell_print(sh, "%s trailer: magic %u swap_type %u copy_done %u image_ok %u", name,
		    st.magic, st.swap_type, st.copy_done, st.image_ok);
}

#if HAVE_UPDATE
static void print_updater(const struct shell *sh)
{
	static struct system_updater_state s;
	char from[SYSTEM_UPDATE_VERSION_STR_LEN] = "-";
	char to[SYSTEM_UPDATE_VERSION_STR_LEN] = "-";

	if (!update_ready) {
		shell_print(sh, "updater not started");
		return;
	}
	/* Both stores' uploads: one upload across targets blocks the other (409 busy). */
	{
		static struct sys_img_upload sys;

		if (sys_img_current(k_uptime_get(), &sys) == 0) {
			shell_print(sh, "STM32 upload %s: %s, %u of %u bytes, version %s", sys.id,
				    sys_img_state_str(sys.state), sys.received_bytes, sys.size_bytes,
				    sys.has_image ? sys.version : "-");
		} else {
			shell_print(sh, "STM32 upload: none");
		}
	}
#if defined(CONFIG_FIRMWARE_STORE)
	{
		static struct fw_upload fw;

		if (fw_store_current(k_uptime_get(), &fw) == 0) {
			shell_print(sh, "ESP32 upload %s: %s, %u of %u bytes, file %s", fw.id,
				    fw_upload_state_str(fw.state), fw.received_bytes, fw.size_bytes,
				    fw.filename);
		} else {
			shell_print(sh, "ESP32 upload: none");
		}
	}
#endif
	system_updater_get_state(&s);
	shell_print(sh, "updater: active %d phase %s job %s upload %s", s.active,
		    s.active ? system_update_phase_str(s.phase) : "-", s.job_id, s.upload_id);
	if (s.confirm_pending) {
		shell_print(sh, "self-confirmation in %u s", s.confirm_remaining_seconds);
	}
	if (s.has_last) {
		if (s.last.has_from_version) {
			(void)system_image_version_str(&s.last.from_version, from, sizeof(from));
		}
		if (s.last.has_version) {
			(void)system_image_version_str(&s.last.version, to, sizeof(to));
		}
		shell_print(sh, "last update %s: %s, %s -> %s%s%s", s.last.job_id,
			    system_update_outcome_str(s.last.state), from, to,
			    s.last.has_error ? ", error " : "",
			    s.last.has_error ? s.last.error_code : "");
	}
}
#endif

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct image_info img;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	print_reset_cause(sh, reset_cause);
	(void)read_image(SLOT0_ID, &img);
	print_image(sh, "slot 1 (running)", &img);
	print_swap_state(sh, "slot 1", SLOT0_ID);
	(void)read_image(SLOT1_ID, &img);
	print_image(sh, "slot 2", &img);
	print_swap_state(sh, "slot 2", SLOT1_ID);
	/* BOOT_SWAP_TYPE_NONE 1, TEST 2, PERM 3, REVERT 4, FAIL 5 */
	shell_print(sh, "running image %s, next swap type %d", confirmed_str(), boot_swap_type());
#if defined(CONFIG_APP_IWDG) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(iwdg))
	shell_print(sh, "iwdg %s, %u ms, %d feeds", atomic_get(&wdt_running) ? "running" : "off",
		    CONFIG_APP_IWDG_TIMEOUT_MS, (int)atomic_get(&wdt_feeds));
#else
	shell_print(sh, "iwdg not built");
#endif
#if HAVE_UPDATE
	print_updater(sh);
#endif
	return 0;
}

static int erase_trailer(const struct flash_area *fa)
{
	return flash_area_erase(fa, fa->fa_size - TRAILER_SECTOR_BYTES, TRAILER_SECTOR_BYTES);
}

static int cmd_erase_trailer(const struct shell *sh, size_t argc, char **argv)
{
	const struct flash_area *fa;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = flash_area_open(SLOT1_ID, &fa);
	if (rc == 0) {
		rc = erase_trailer(fa);
		flash_area_close(fa);
	}
	shell_print(sh, "slot 2 trailer erase: %d", rc);
	return rc;
}

/* Copy the running image into slot 2: a swap of two identical images checks the
 * whole path but the change of version. Bypasses system-image-store: debug only. */
static int cmd_selfcopy(const struct shell *sh, size_t argc, char **argv)
{
	const struct flash_area *src;
	const struct flash_area *dst;
	struct image_info img;
	int64_t t0 = k_uptime_get();
	uint32_t erase_len;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = read_image(SLOT0_ID, &img);
	if (rc != 0 || !img.valid) {
		shell_error(sh, "running image unreadable: %d", rc);
		return -EIO;
	}
	rc = flash_area_open(SLOT0_ID, &src);
	if (rc != 0) {
		return rc;
	}
	rc = flash_area_open(SLOT1_ID, &dst);
	if (rc != 0) {
		flash_area_close(src);
		return rc;
	}
	if (img.total > dst->fa_size - TRAILER_SECTOR_BYTES) {
		shell_error(sh, "image of %u bytes does not fit slot 2", img.total);
		rc = -EFBIG;
		goto out;
	}
	rc = erase_trailer(dst);
	erase_len = ROUND_UP(img.total, SECTOR_BYTES);
	if (rc == 0) {
		rc = flash_area_erase(dst, 0, erase_len);
	}
	for (uint32_t off = 0; rc == 0 && off < img.total; off += SECTOR_BYTES) {
		size_t len = MIN(SECTOR_BYTES, img.total - off);

		rc = flash_area_read(src, off, copy_buf, len);
		if (rc == 0) {
			rc = flash_area_write(dst, off, copy_buf, len);
		}
	}
	shell_print(sh, "copied %u bytes into slot 2 in %lld ms: %d", img.total,
		    k_uptime_get() - t0, rc);
out:
	flash_area_close(dst);
	flash_area_close(src);
	return rc;
}

static int cmd_request(const struct shell *sh, size_t argc, char **argv)
{
	bool permanent = argc > 1 && strcmp(argv[1], "perm") == 0;
	int rc = boot_set_pending(permanent ? 1 : 0);

	shell_print(sh, "request %s: %d, next swap type %d", permanent ? "perm" : "test", rc,
		    boot_swap_type());
	return rc;
}

/* The owner's console command: confirm the running image now. Through
 * system-updater when it runs, so last_update follows; the shell thread's stack
 * is in SRAM, which the XIP-safe write requires. */
static int cmd_confirm(const struct shell *sh, size_t argc, char **argv)
{
	int64_t t0 = k_uptime_get();
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if HAVE_UPDATE
	rc = update_ready ? system_updater_confirm_now() : confirm_running();
#else
	rc = confirm_running();
#endif
	shell_print(sh, "confirm: %d in %lld ms, running image %s", rc, k_uptime_get() - t0,
		    confirmed_str());
	return rc;
}

/* bootutil directly, bypassing system-updater (debug of the write itself). */
static int cmd_confirm_raw(const struct shell *sh, size_t argc, char **argv)
{
	int64_t t0 = k_uptime_get();
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = confirm_running();
	shell_print(sh, "confirm-raw: %d in %lld ms, running image %s", rc, k_uptime_get() - t0,
		    confirmed_str());
	return rc;
}

static int cmd_hang(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "hanging with interrupts locked");
	k_msleep(100);
	(void)irq_lock();
	for (;;) {
	}
	return 0;
}

/* "255.255.65535+4294967295" and its terminator. */
static char running_version[25];

const char *system_service_running_version(void)
{
	return running_version[0] != '\0' ? running_version : NULL;
}

void system_service_start(void)
{
	struct image_info img;
	int rc = read_image(SLOT0_ID, &img);

	LOG_INF("reset cause 0x%08x%s", reset_cause,
		(reset_cause & RESET_WATCHDOG) != 0U ? " (watchdog)" : "");
	if (rc != 0 || !img.valid) {
		LOG_WRN("running image header unreadable: %d", rc);
	} else {
		(void)snprintf(running_version, sizeof(running_version), "%u.%u.%u+%u", img.major,
			       img.minor, img.revision, img.build);
		LOG_INF("running image %s, %u bytes, %s, next swap type %d", running_version,
			img.total, confirmed_str(), boot_swap_type());
	}
	update_start();
}

SHELL_STATIC_SUBCMD_SET_CREATE(sysupd_cmds,
	SHELL_CMD(status, NULL, "Slots, trailers, confirmation, reset cause, IWDG, updater",
		  cmd_status),
	SHELL_CMD(selfcopy, NULL, "Copy the running image into slot 2 (bypasses the store)",
		  cmd_selfcopy),
	SHELL_CMD(erase-trailer, NULL, "Erase the last 64 KiB of slot 2", cmd_erase_trailer),
	SHELL_CMD_ARG(request, NULL, "Request a swap: [test|perm]", cmd_request, 1, 1),
	SHELL_CMD(confirm, NULL, "Confirm the running image now (through the updater)",
		  cmd_confirm),
	SHELL_CMD(confirm-raw, NULL, "Confirm through bootutil directly (writes slot 1 via XIP)",
		  cmd_confirm_raw),
	SHELL_CMD(hang, NULL, "Lock interrupts and spin (watchdog test)", cmd_hang),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sysupd, &sysupd_cmds, "STM32 firmware update (debug)", NULL);
