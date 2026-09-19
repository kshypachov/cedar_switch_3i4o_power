/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Debug shell: where LittleFS time goes on the SPI NOR (owner's request
 * 2026-09-19, reports/littlefs-speed).
 *
 *   lfsbench raw [kib] [sram]     raw flash_area_read() of the LittleFS partition
 *                                 in 16 B and 256 B reads (at most 64 KiB), 4 KiB
 *                                 and 32 KiB reads (read-only);
 *                                 sram: into a malloc() buffer (the malloc arena is
 *                                 in SRAM), otherwise into a static one (PSRAM)
 *   lfsbench rawwrite [kib] confirm
 *                                 erase + program the start of slot1_partition
 *                                 (slot 2 of the update) in 4 KiB steps, then
 *                                 erase it again: destroys a staged upload
 *   lfsbench file [kib] [chunk]   /lfs/bench.bin: write kib KiB in chunk-byte
 *                                 writes + fs_sync, read it back, unlink
 *   lfsbench meta [rounds] [inplace]
 *                                 the image store's metadata save: open tmp,
 *                                 write 128 B, fs_sync, close, rename; inplace:
 *                                 the same file rewritten, no tmp and no rename
 *   lfsbench statvfs              one fs_statvfs() of /lfs
 *
 * Every line gives the time and, where it applies, KB/s (1 KB = 1000 B).
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#if defined(CONFIG_SHELL) && defined(CONFIG_FILE_SYSTEM_LITTLEFS) &&                            \
	FIXED_PARTITION_EXISTS(storage_partition) && FIXED_PARTITION_EXISTS(slot1_partition)

#define BENCH_FILE "/lfs/bench.bin"
#define BENCH_TMP  "/lfs/bench.tmp"
#define BENCH_META "/lfs/bench.meta"
#define BUF_SIZE   32768U

static uint8_t buf[BUF_SIZE];

static int64_t now_us(void)
{
	return (int64_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

/* KB/s with one decimal, as tenths */
static uint32_t kbps_x10(uint64_t bytes, int64_t us)
{
	return us > 0 ? (uint32_t)(bytes * 10000U / (uint64_t)us) : 0U;
}

static void print_rate(const struct shell *sh, const char *what, uint64_t bytes, int64_t us)
{
	const uint32_t r = kbps_x10(bytes, us);

	shell_print(sh, "%-28s %8lld us  %6u.%u KB/s", what, us, r / 10U, r % 10U);
}

static int cmd_raw(const struct shell *sh, size_t argc, char **argv)
{
	static const uint32_t sizes[] = {16U, 256U, 4096U, 32768U};
	const uint32_t kib = argc > 1 ? strtoul(argv[1], NULL, 0) : 256U;
	const bool sram = argc > 2 && strcmp(argv[2], "sram") == 0;
	uint8_t *const mem = sram ? malloc(BUF_SIZE) : buf;
	const struct flash_area *fa;
	int rc;

	if (mem == NULL) {
		shell_error(sh, "no %u B for the SRAM buffer", BUF_SIZE);
		return -ENOMEM;
	}
	shell_print(sh, "buffer at %p", (void *)mem);
	rc = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);

	if (rc != 0) {
		shell_error(sh, "open storage_partition: %d", rc);
		return rc;
	}
	if (kib == 0U || kib * 1024U > fa->fa_size) {
		flash_area_close(fa);
		if (sram) {
			free(mem);
		}
		shell_error(sh, "kib 1..%u", (uint32_t)(fa->fa_size / 1024U));
		return -EINVAL;
	}
	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		/* small reads over at most 64 KiB, or they take minutes */
		const uint32_t total = sizes[i] < 4096U ? MIN(kib * 1024U, 65536U) : kib * 1024U;
		const int64_t t0 = now_us();
		char what[32];

		uint32_t crc = 0U;
		int64_t us;

		for (uint32_t off = 0; off < total && rc == 0; off += sizes[i]) {
			rc = flash_area_read(fa, off, mem, MIN(sizes[i], total - off));
		}
		us = now_us() - t0;
		snprintf(what, sizeof(what), "raw read %u B x %u", sizes[i], total / sizes[i]);
		print_rate(sh, what, total, us);
		shell_print(sh, "%-28s %8lld us", "  per read", us / (int64_t)(total / sizes[i]));
		if (rc != 0) {
			shell_error(sh, "flash_area_read error %d", rc);
			break;
		}
		/* the same bytes read again, so every mode can be checked against another */
		for (uint32_t off = 0; off < total && rc == 0; off += 4096U) {
			rc = flash_area_read(fa, off, mem, MIN(4096U, total - off));
			crc = crc32_ieee_update(crc, mem, MIN(4096U, total - off));
		}
		if (i == ARRAY_SIZE(sizes) - 1U) {
			shell_print(sh, "crc32 of %u KiB: %08x", kib, crc);
		}
	}
	flash_area_close(fa);
	if (sram) {
		free(mem);
	}
	return rc;
}

static int cmd_rawwrite(const struct shell *sh, size_t argc, char **argv)
{
	const uint32_t kib = argc > 1 ? strtoul(argv[1], NULL, 0) : 64U;
	const struct flash_area *fa;
	int64_t erase_us = 0;
	int64_t prog_us = 0;
	uint32_t bad = 0U;
	int rc;

	if (argc < 3 || strcmp(argv[2], "confirm") != 0) {
		shell_error(sh, "destroys a staged update: lfsbench rawwrite <kib> confirm");
		return -EINVAL;
	}
	rc = flash_area_open(FIXED_PARTITION_ID(slot1_partition), &fa);
	if (rc != 0) {
		shell_error(sh, "open slot1_partition: %d", rc);
		return rc;
	}
	if (kib == 0U || kib % 4U != 0U || kib * 1024U > fa->fa_size / 2U) {
		flash_area_close(fa);
		shell_error(sh, "kib: a multiple of 4, at most half the slot");
		return -EINVAL;
	}
	for (uint32_t i = 0; i < 4096U; i++) {
		buf[i] = (uint8_t)(i * 7U + 3U);
	}
	for (uint32_t off = 0; off < kib * 1024U && rc == 0; off += 4096U) {
		int64_t t0 = now_us();

		rc = flash_area_erase(fa, off, 4096U);
		erase_us += now_us() - t0;
		if (rc == 0) {
			t0 = now_us();
			rc = flash_area_write(fa, off, buf, 4096U);
			prog_us += now_us() - t0;
		}
		/* read back into the second half of buf: checks the write and read paths */
		if (rc == 0) {
			rc = flash_area_read(fa, off, buf + 4096U, 4096U);
		}
		if (rc == 0 && memcmp(buf, buf + 4096U, 4096U) != 0) {
			bad++;
		}
	}
	shell_print(sh, "%u sectors of 4 KiB, %u read back wrong", kib / 4U, bad);
	print_rate(sh, "erase 4 KiB (total)", kib * 1024U, erase_us);
	print_rate(sh, "program 4 KiB (total)", kib * 1024U, prog_us);
	print_rate(sh, "erase + program", kib * 1024U, erase_us + prog_us);
	(void)flash_area_erase(fa, 0, kib * 1024U);
	flash_area_close(fa);
	if (rc != 0) {
		shell_error(sh, "flash: %d", rc);
	}
	return rc;
}

static int cmd_file(const struct shell *sh, size_t argc, char **argv)
{
	const uint32_t kib = argc > 1 ? strtoul(argv[1], NULL, 0) : 128U;
	const uint32_t chunk = argc > 2 ? strtoul(argv[2], NULL, 0) : 4096U;
	const uint32_t total = kib * 1024U;
	struct fs_file_t f;
	int64_t t0;
	int64_t t1;
	int rc;

	if (kib == 0U || chunk == 0U || chunk > BUF_SIZE) {
		shell_error(sh, "chunk 1..%u", BUF_SIZE);
		return -EINVAL;
	}
	for (uint32_t i = 0; i < chunk; i++) {
		buf[i] = (uint8_t)(i * 13U + 1U);
	}
	(void)fs_unlink(BENCH_FILE);

	fs_file_t_init(&f);
	t0 = now_us();
	rc = fs_open(&f, BENCH_FILE, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	for (uint32_t off = 0; off < total && rc == 0; off += chunk) {
		const ssize_t n = fs_write(&f, buf, MIN(chunk, total - off));

		rc = n < 0 ? (int)n : 0;
	}
	t1 = now_us();
	if (rc == 0) {
		rc = fs_sync(&f);
	}
	(void)fs_close(&f);
	if (rc != 0) {
		shell_error(sh, "write: %d", rc);
		return rc;
	}
	shell_print(sh, "file %u KiB in %u B writes", kib, chunk);
	print_rate(sh, "write (without sync)", total, t1 - t0);
	print_rate(sh, "write + sync + close", total, now_us() - t0);

	fs_file_t_init(&f);
	t0 = now_us();
	rc = fs_open(&f, BENCH_FILE, FS_O_READ);
	for (uint32_t off = 0; off < total && rc == 0; off += chunk) {
		const ssize_t n = fs_read(&f, buf, MIN(chunk, total - off));

		rc = n < 0 ? (int)n : (n == 0 ? -EIO : 0);
	}
	(void)fs_close(&f);
	print_rate(sh, "read", total, now_us() - t0);

	t0 = now_us();
	(void)fs_unlink(BENCH_FILE);
	shell_print(sh, "%-28s %8lld us", "unlink", now_us() - t0);
	return rc;
}

static int cmd_meta(const struct shell *sh, size_t argc, char **argv)
{
	const uint32_t rounds = argc > 1 ? strtoul(argv[1], NULL, 0) : 20U;
	const bool inplace = argc > 2 && strcmp(argv[2], "inplace") == 0;
	const char *const path = inplace ? BENCH_META : BENCH_TMP;
	int64_t t_open = 0, t_write = 0, t_sync = 0, t_close = 0, t_rename = 0;
	int64_t best = INT64_MAX, worst = 0;
	int rc = 0;

	memset(buf, 0x5a, 128);
	for (uint32_t i = 0; i < rounds && rc == 0; i++) {
		struct fs_file_t f;
		const int64_t t0 = now_us();
		int64_t a, b, c, d, e;

		fs_file_t_init(&f);
		rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
		a = now_us();
		if (rc == 0) {
			buf[0] = (uint8_t)i;
			rc = fs_write(&f, buf, 128) == 128 ? 0 : -EIO;
		}
		b = now_us();
		if (rc == 0) {
			rc = fs_sync(&f);
		}
		c = now_us();
		(void)fs_close(&f);
		d = now_us();
		if (rc == 0 && !inplace) {
			rc = fs_rename(BENCH_TMP, BENCH_META);
		}
		e = now_us();
		t_open += a - t0;
		t_write += b - a;
		t_sync += c - b;
		t_close += d - c;
		t_rename += e - d;
		best = MIN(best, e - t0);
		worst = MAX(worst, e - t0);
	}
	(void)fs_unlink(BENCH_META);
	if (rc != 0) {
		shell_error(sh, "round failed: %d", rc);
		return rc;
	}
	shell_print(sh, "metadata save%s x %u, mean us: open %lld write %lld sync %lld close %lld "
		    "rename %lld", inplace ? " (in place)" : "", rounds, t_open / rounds, t_write / rounds, t_sync / rounds,
		    t_close / rounds, t_rename / rounds);
	shell_print(sh, "per save: mean %lld us, best %lld, worst %lld",
		    (t_open + t_write + t_sync + t_close + t_rename) / rounds, best, worst);
	return 0;
}

static int cmd_statvfs(const struct shell *sh, size_t argc, char **argv)
{
	struct fs_statvfs st;
	const int64_t t0 = now_us();
	const int rc = fs_statvfs("/lfs", &st);

	if (rc != 0) {
		shell_error(sh, "statvfs: %d", rc);
		return rc;
	}
	shell_print(sh, "statvfs %lld us: %lu of %lu blocks free", now_us() - t0,
		    (unsigned long)st.f_bfree, (unsigned long)st.f_blocks);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	lfsbench_cmds,
	SHELL_CMD_ARG(raw, NULL, "raw read of the LittleFS partition: [kib] [sram]", cmd_raw, 1, 2),
	SHELL_CMD_ARG(rawwrite, NULL, "erase+program slot 2: <kib> confirm", cmd_rawwrite, 3, 0),
	SHELL_CMD_ARG(file, NULL, "file write/read: [kib] [chunk]", cmd_file, 1, 2),
	SHELL_CMD_ARG(meta, NULL, "metadata save cycle: [rounds] [inplace]", cmd_meta, 1, 2),
	SHELL_CMD(statvfs, NULL, "one fs_statvfs of /lfs", cmd_statvfs),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(lfsbench, &lfsbench_cmds, "LittleFS / SPI NOR speed", NULL);

#endif
