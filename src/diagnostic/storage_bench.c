/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Debug shell: time the settings file backend and the LittleFS reads under it.
 *
 *   storage_bench [rounds] [key]
 *
 * key is the entry timed as the hit, reg/auth/admin_verifier by default. That
 * one exists only once the web password is set, so after storage_wipe pass a
 * key Matter always writes, e.g. mt/cfg/unique-id.
 *
 * Added in P3 to find where Matter's storage time goes: with two fabrics its
 * initialisation took minutes, though /lfs/settings held 4.4 KB. Each line is
 * the best and the mean of the rounds, in microseconds:
 *
 *   open+close          fs_open and fs_close of the settings file
 *   read 32 +seek       the whole file in 32-byte reads with a seek before each,
 *                       the pattern of subsys/settings/src/settings_file.c
 *   read 32             the same without the seeks
 *   read 1024           the whole file in 1024-byte reads
 *   scan (no match)     settings_load_subtree_direct() of a key that is not
 *                       there: what one missing key costs Matter's KVS
 *   scan (all)          settings_load_subtree_direct(NULL): every entry
 *
 * Every backend:
 *
 *   load_one (hit)      settings_load_one() of an existing key
 *   load_one (miss)     settings_load_one() of a key that is not there
 *   val_len (hit/miss)  settings_get_val_len(), Matter's existence check
 *
 * Matter's Zephyr KVS reads keys with these two calls since Zephyr 4.4; a
 * backend without csi_load_one/csi_get_val_len answers them with a full load.
 *
 * Read-only; safe while the rest of the firmware runs.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>

#if defined(CONFIG_SHELL) && defined(CONFIG_SETTINGS)

#define BENCH_HIT_KEY  "reg/auth/admin_verifier"
#define BENCH_MISS_KEY "zz/storage/bench/missing"

static int64_t now_us(void)
{
	return (int64_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

#if defined(CONFIG_SETTINGS_FILE)
#define BENCH_PATH CONFIG_SETTINGS_FILE_PATH

static int64_t bench_open_close(void)
{
	struct fs_file_t file;
	int64_t t0;

	fs_file_t_init(&file);
	t0 = now_us();
	if (fs_open(&file, BENCH_PATH, FS_O_READ) != 0) {
		return -1;
	}
	(void)fs_close(&file);
	return now_us() - t0;
}

static int64_t bench_read(size_t chunk, bool seek_each, size_t *total)
{
	struct fs_file_t file;
	char buf[1024];
	off_t off = 0;
	int64_t t0;

	fs_file_t_init(&file);
	if (fs_open(&file, BENCH_PATH, FS_O_READ) != 0) {
		return -1;
	}
	t0 = now_us();
	for (;;) {
		ssize_t r;

		if (seek_each && fs_seek(&file, off, FS_SEEK_SET) != 0) {
			break;
		}
		r = fs_read(&file, buf, MIN(chunk, sizeof(buf)));
		if (r <= 0) {
			break;
		}
		off += r;
	}
	int64_t dt = now_us() - t0;

	(void)fs_close(&file);
	*total = (size_t)off;
	return dt;
}

#endif /* CONFIG_SETTINGS_FILE */

static int count_entry(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		       void *param)
{
	ARG_UNUSED(key);
	ARG_UNUSED(len);
	ARG_UNUSED(read_cb);
	ARG_UNUSED(cb_arg);
	(*(int *)param)++;
	return 0;
}

static int64_t bench_scan(const char *subtree, int *entries)
{
	int64_t t0 = now_us();

	*entries = 0;
	(void)settings_load_subtree_direct(subtree, count_entry, entries);
	return now_us() - t0;
}

struct stat_line {
	int64_t best;
	int64_t sum;
};

static void add(struct stat_line *s, int64_t v)
{
	if (s->best == 0 || v < s->best) {
		s->best = v;
	}
	s->sum += v;
}

static int64_t bench_load_one(const char *key, ssize_t *result)
{
	uint8_t buf[128];
	int64_t t0 = now_us();

	*result = settings_load_one(key, buf, sizeof(buf));
	return now_us() - t0;
}

static int64_t bench_val_len(const char *key, ssize_t *result)
{
	int64_t t0 = now_us();

	*result = settings_get_val_len(key);
	return now_us() - t0;
}

static int cmd_storage_bench(const struct shell *sh, size_t argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 3;
	const char *hit_key = argc > 2 ? argv[2] : BENCH_HIT_KEY;
	struct stat_line miss = {0}, all = {0};
	struct stat_line one_hit = {0}, one_miss = {0}, len_hit = {0}, len_miss = {0};
	int entries = 0;
	int none = 0;
	ssize_t hit_len = 0;
	ssize_t miss_len = 0;
	ssize_t val_hit = 0;
	ssize_t val_miss = 0;

	if (rounds < 1 || rounds > 50) {
		shell_error(sh, "rounds: 1..50");
		return -EINVAL;
	}
#if defined(CONFIG_SETTINGS_FILE)
	struct stat_line oc = {0}, r32s = {0}, r32 = {0}, r1k = {0};
	size_t size = 0;
#endif
	for (int i = 0; i < rounds; i++) {
#if defined(CONFIG_SETTINGS_FILE)
		add(&oc, bench_open_close());
		add(&r32s, bench_read(32, true, &size));
		add(&r32, bench_read(32, false, &size));
		add(&r1k, bench_read(1024, false, &size));
#endif
		add(&miss, bench_scan("zz-storage-bench-missing", &none));
		add(&all, bench_scan(NULL, &entries));
		add(&one_hit, bench_load_one(hit_key, &hit_len));
		add(&one_miss, bench_load_one(BENCH_MISS_KEY, &miss_len));
		add(&len_hit, bench_val_len(hit_key, &val_hit));
		add(&len_miss, bench_val_len(BENCH_MISS_KEY, &val_miss));
	}
#if defined(CONFIG_SETTINGS_FILE)
	shell_print(sh, "file %s: %zu bytes, %d entries, %d rounds (best / mean, us)", BENCH_PATH,
		    size, entries, rounds);
	shell_print(sh, "  open+close       %8lld / %8lld", oc.best, oc.sum / rounds);
	shell_print(sh, "  read 32 +seek    %8lld / %8lld", r32s.best, r32s.sum / rounds);
	shell_print(sh, "  read 32          %8lld / %8lld", r32.best, r32.sum / rounds);
	shell_print(sh, "  read 1024        %8lld / %8lld", r1k.best, r1k.sum / rounds);
#else
	shell_print(sh, "settings backend: %d entries, %d rounds (best / mean, us)", entries, rounds);
#endif
	shell_print(sh, "  scan (no match)  %8lld / %8lld", miss.best, miss.sum / rounds);
	shell_print(sh, "  scan (all)       %8lld / %8lld", all.best, all.sum / rounds);
	shell_print(sh, "  load_one (hit)   %8lld / %8lld  -> %zd", one_hit.best, one_hit.sum / rounds,
		    hit_len);
	shell_print(sh, "  load_one (miss)  %8lld / %8lld  -> %zd", one_miss.best,
		    one_miss.sum / rounds, miss_len);
	shell_print(sh, "  val_len (hit)    %8lld / %8lld  -> %zd", len_hit.best, len_hit.sum / rounds,
		    val_hit);
	shell_print(sh, "  val_len (miss)   %8lld / %8lld  -> %zd", len_miss.best,
		    len_miss.sum / rounds, val_miss);
	return 0;
}

/*
 * storage_wipe yes: erase the whole partition of the settings backend, for
 * measurements that must start from an empty partition. Reboot right after:
 * whatever the backend keeps in RAM no longer matches the flash.
 */
#if DT_HAS_CHOSEN(zephyr_settings_partition)
#define WIPE_PARTITION DT_PARTITION_ID(DT_CHOSEN(zephyr_settings_partition))
#else
#define WIPE_PARTITION PARTITION_ID(storage_partition)
#endif

static int cmd_storage_wipe(const struct shell *sh, size_t argc, char **argv)
{
	const struct flash_area *fa;
	int64_t t0;
	int rc;

	if (argc < 2 || strcmp(argv[1], "yes") != 0) {
		shell_error(sh, "storage_wipe yes - erases the settings partition; reboot afterwards");
		return -EINVAL;
	}
	rc = flash_area_open(WIPE_PARTITION, &fa);
	if (rc != 0) {
		shell_error(sh, "flash_area_open: %d", rc);
		return rc;
	}
	t0 = now_us();
	rc = flash_area_erase(fa, 0, fa->fa_size);
	shell_print(sh, "storage_wipe: offset 0x%lx size %zu erased rc=%d in %lld ms",
		    (unsigned long)fa->fa_off, fa->fa_size, rc, (now_us() - t0) / 1000);
	flash_area_close(fa);
	return rc;
}

SHELL_CMD_ARG_REGISTER(storage_wipe, NULL, "Erase the settings partition: storage_wipe yes",
		       cmd_storage_wipe, 2, 0);

SHELL_CMD_ARG_REGISTER(storage_bench, NULL,
		       "Time settings/LittleFS reads: storage_bench [rounds] [key]",
		       cmd_storage_bench, 1, 2);

#endif
