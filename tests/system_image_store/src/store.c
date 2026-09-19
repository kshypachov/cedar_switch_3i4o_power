/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * system-image-store: the upload lifecycle over a slot on the sim flash and
 * metadata on LittleFS - the errno contract, the sector-erase rule and its
 * repair, resending after a crash, recovery after a "reboot" (remount and init),
 * the slot lock, expiry.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <system_image_store/system_image_store.h>

#include "common.h"

#define EXPIRE_MS ((int64_t)CONFIG_SYSTEM_IMAGE_STORE_EXPIRE_SECONDS * 1000)
#define CHUNK     CONFIG_SYSTEM_IMAGE_STORE_CHUNK_MAX
#define META      TEST_DIR "/sysimg.meta"
#define TMP       TEST_DIR "/sysimg.meta.tmp"

/* Offsets in the packed metadata record (system_image_store.c, struct meta). */
#define META_STATE    6
#define META_ID       8
#define META_FILENAME 32
#define META_SIZE     545
#define META_RECEIVED 549

static struct sys_img_upload up;
static uint8_t digest[32];
static uint8_t big[CHUNK + 1];
static uint8_t cmp[4096];

/* -- helpers ------------------------------------------------------------------------ */

static bool exists(const char *path)
{
	struct fs_dirent e;

	return fs_stat(path, &e) == 0;
}

static void reboot(int64_t now)
{
	volume_remount();
	zassert_ok(sys_img_init(TEST_DIR, &platform, now));
}

static void create_for(const struct fixture *fx, int64_t now)
{
	sha256_of(fx->data, fx->len, digest);
	zassert_ok(sys_img_create("cedar.signed.bin", (uint32_t)fx->len, digest, now, &up));
}

static void send(const struct fixture *fx, size_t from, size_t to, size_t chunk, int64_t now)
{
	for (size_t off = from; off < to; off += chunk) {
		const size_t n = MIN(chunk, to - off);

		zassert_ok(sys_img_chunk_accept(up.id, (uint32_t)off, &fx->data[off], n, now),
			   "offset %zu", off);
		zassert_ok(sys_img_chunk_commit(up.id, now), "offset %zu", off);
	}
}

static void upload(const struct fixture *fx, int64_t now)
{
	create_for(fx, now);
	send(fx, 0, fx->len, CHUNK, now);
}

static void verify(int64_t now)
{
	zassert_ok(sys_img_verify_begin(up.id, now));
	zassert_ok(sys_img_verify(up.id, NULL, NULL, NULL, now));
	zassert_ok(sys_img_get(up.id, now, &up));
}

static void assert_slot(const uint8_t *data, size_t from, size_t to)
{
	for (size_t off = from; off < to; off += sizeof(cmp)) {
		const size_t n = MIN(sizeof(cmp), to - off);

		slot_raw_read((uint32_t)off, cmp, n);
		zassert_mem_equal(cmp, &data[off], n, "slot differs in [0x%zx, 0x%zx)", off,
				  off + n);
	}
}

static void assert_erased(uint32_t from, uint32_t to)
{
	for (uint32_t off = from; off < to; off++) {
		uint8_t b;

		slot_raw_read(off, &b, 1);
		zassert_equal(b, 0xFF, "slot byte 0x%x not erased", off);
	}
}

static bool erased_sector(uint32_t sector_off)
{
	for (int i = 0; i < MIN(slot.erase_calls, (int)ARRAY_SIZE(slot.erase_off)); i++) {
		if (sector_off >= slot.erase_off[i] &&
		    sector_off < slot.erase_off[i] + slot.erase_len[i]) {
			return true;
		}
	}
	return false;
}

static size_t read_file(const char *path, uint8_t *buf, size_t cap)
{
	struct fs_file_t f;

	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, path, FS_O_READ));
	ssize_t n = fs_read(&f, buf, cap);

	zassert_true(n >= 0);
	zassert_ok(fs_close(&f));
	return (size_t)n;
}

static void write_file(const char *path, const uint8_t *data, size_t len)
{
	struct fs_file_t f;

	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
	zassert_equal(fs_write(&f, data, len), (ssize_t)len);
	zassert_ok(fs_close(&f));
}

/* Change the metadata record with @p edit and give it a valid CRC again. */
static void tamper_meta(void (*edit)(uint8_t *m, size_t len))
{
	static uint8_t m[2048];
	size_t len = read_file(META, m, sizeof(m));

	zassert_true(len > META_RECEIVED + 4U);
	edit(m, len);
	sys_put_le32(crc32_ieee(m, len - 4U), &m[len - 4U]);
	write_file(META, m, len);
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	slot_stream(false);
	slot_reset_knobs();
	slot_erase_all();
	volume_fresh();
	zassert_ok(sys_img_init(TEST_DIR, &platform, 0));
	memset(&up, 0, sizeof(up));
	slot_reset_knobs();
}

ZTEST_SUITE(system_image_store, NULL, NULL, before, NULL, NULL);

/* -- init ---------------------------------------------------------------------------- */

ZTEST(system_image_store, test_init_rejects_bad_platform)
{
	struct sys_img_platform bad;
	char long_dir[100];

	memset(long_dir, 'd', sizeof(long_dir) - 1U);
	long_dir[sizeof(long_dir) - 1U] = '\0';
	zassert_equal(sys_img_init(NULL, &platform, 0), -EINVAL);
	zassert_equal(sys_img_init("", &platform, 0), -EINVAL);
	zassert_equal(sys_img_init(long_dir, &platform, 0), -EINVAL);
	zassert_equal(sys_img_init(TEST_DIR, NULL, 0), -EINVAL);

	bad = platform;
	bad.read = NULL;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.write = NULL;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.erase = NULL;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.slot_locked = NULL;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.sector_size = 3000;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.sector_size = 0;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.sector_size = CONFIG_SYSTEM_IMAGE_STORE_SECTOR_MAX * 2U;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.trailer_bytes = TRAILER + 1U;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.trailer_bytes = 0;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.slot_size = SLOT_SIZE + 1U;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.slot_size = TRAILER;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	/* consistent sizes, but a sector that is not a power of two */
	bad = platform;
	bad.sector_size = 3000;
	bad.trailer_bytes = 3000U * 22U;
	bad.slot_size = 3000U * 1400U;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	bad = platform;
	bad.image.header_size = 31;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);

	/* a failed init leaves the store closed - but only when the call reached it */
	bad = platform;
	bad.slot_size = TRAILER;
	zassert_equal(sys_img_init(TEST_DIR, &bad, 0), -EINVAL);
	zassert_ok(sys_img_init(TEST_DIR, &platform, 0));
	/* the smallest valid geometry */
	bad = platform;
	bad.slot_size = TRAILER + SECTOR;
	bad.image.header_size = 32;
	zassert_ok(sys_img_init(TEST_DIR, &bad, 0));
	zassert_ok(sys_img_init(TEST_DIR, &platform, 0));
}

ZTEST(system_image_store, test_closed_store_after_failed_init)
{
	/* the slot unreadable during recovery of a ready upload */
	upload(fixture("valid"), 0);
	verify(0);
	slot.fail_read_in = 1;
	volume_remount();
	zassert_equal(sys_img_init(TEST_DIR, &platform, 0), -EIO);
	slot_reset_knobs();

	zassert_equal(sys_img_create("x", 10, digest, 0, NULL), -EAGAIN);
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);
	zassert_equal(sys_img_current(0, NULL), -ENOENT);
	zassert_equal(sys_img_chunk_accept(up.id, 0, digest, 1, 0), -ENOENT);
	zassert_equal(sys_img_chunk_commit(up.id, 0), -ENOENT);
	zassert_equal(sys_img_verify_begin(up.id, 0), -ENOENT);
	zassert_equal(sys_img_verify(up.id, NULL, NULL, NULL, 0), -ENOENT);
	zassert_equal(sys_img_delete(up.id), -ENOENT);
	zassert_equal(sys_img_set_in_use(up.id, true), -ENOENT);
	zassert_equal(sys_img_set_active_job(up.id, "job"), -ENOENT);
	zassert_equal(sys_img_image_info(up.id, NULL), -ENOENT);
	sys_img_tick(EXPIRE_MS * 2);

	/* opens again, and the upload is still there */
	zassert_ok(sys_img_init(TEST_DIR, &platform, 0));
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_READY);
}

/* -- create and get ------------------------------------------------------------------ */

ZTEST(system_image_store, test_create_starts_receiving)
{
	const struct fixture *fx = fixture("valid");
	struct sys_img_upload got;
	struct fs_dir_t dir;
	struct fs_dirent entry;

	create_for(fx, 5);
	zassert_equal(strlen(up.id), SYS_IMG_ID_LEN);
	zassert_mem_equal(up.id, "sysimg_", 7);
	zassert_str_equal(up.filename, "cedar.signed.bin");
	zassert_equal(up.size_bytes, fx->len);
	zassert_equal(up.received_bytes, 0U);
	zassert_mem_equal(up.sha256, digest, sizeof(digest));
	zassert_equal(up.state, SYS_IMG_RECEIVING);
	zassert_false(up.has_image);
	zassert_false(up.in_use);
	zassert_false(up.chunk_pending);
	zassert_str_equal(up.active_job_id, "");
	zassert_str_equal(up.error_code, "");
	zassert_str_equal(up.version, "");
	zassert_true(exists(META));
	zassert_false(exists(TMP));

	zassert_ok(sys_img_get(up.id, 6, &got));
	zassert_mem_equal(&got, &up, sizeof(got));
	zassert_ok(sys_img_current(6, &got));
	zassert_mem_equal(&got, &up, sizeof(got));
	zassert_ok(sys_img_current(6, NULL));
	zassert_ok(sys_img_get(up.id, 6, NULL));
	zassert_equal(sys_img_get("sysimg_0000000000000000", 6, &got), -ENOENT);
	zassert_equal(sys_img_get(NULL, 6, &got), -ENOENT);

	/* Nothing in firmware-store's namespace. */
	fs_dir_t_init(&dir);
	zassert_ok(fs_opendir(&dir, TEST_DIR));
	while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		zassert_not_equal(strncmp(entry.name, "upload", 6), 0, "%s", entry.name);
	}
	zassert_ok(fs_closedir(&dir));

	/* A second create makes a different id. */
	zassert_equal(sys_img_delete(up.id), 0);
	create_for(fx, 7);
	zassert_not_equal(strcmp(got.id, up.id), 0);
}

ZTEST(system_image_store, test_current_without_upload)
{
	struct sys_img_upload got;

	zassert_equal(sys_img_current(0, &got), -ENOENT);
}

ZTEST(system_image_store, test_create_erases_the_trailer_only)
{
	const struct fixture *fx = fixture("valid");
	uint8_t b[16];

	slot_raw_fill(MAX_IMAGE, 0x00, TRAILER);
	slot_raw_fill(MAX_IMAGE - 16U, 0x11, 16);
	slot_raw_fill(0, 0x22, 16);
	create_for(fx, 0);

	zassert_equal(slot.erase_calls, 1);
	zassert_equal(slot.erase_off[0], MAX_IMAGE);
	zassert_equal(slot.erase_len[0], TRAILER);
	assert_erased(MAX_IMAGE, SLOT_SIZE);
	slot_raw_read(MAX_IMAGE - 16U, b, 16);
	for (int i = 0; i < 16; i++) {
		zassert_equal(b[i], 0x11);
	}
	slot_raw_read(0, b, 16);
	for (int i = 0; i < 16; i++) {
		zassert_equal(b[i], 0x22);
	}
}

ZTEST(system_image_store, test_create_argument_errors)
{
	static char name[SYS_IMG_FILENAME_MAX + 2];

	sha256_of((const uint8_t *)"x", 1, digest);
	zassert_equal(sys_img_create(NULL, 10, digest, 0, &up), -EINVAL);
	zassert_equal(sys_img_create("", 10, digest, 0, &up), -EINVAL);
	zassert_equal(sys_img_create("a", 0, digest, 0, &up), -EINVAL);
	zassert_equal(sys_img_create("a", 10, NULL, 0, &up), -EINVAL);
	memset(name, 'n', SYS_IMG_FILENAME_MAX + 1U);
	name[SYS_IMG_FILENAME_MAX + 1U] = '\0';
	zassert_equal(sys_img_create(name, 10, digest, 0, &up), -EINVAL);
	zassert_equal(slot.erase_calls, 0);
	zassert_false(exists(META));

	name[SYS_IMG_FILENAME_MAX] = '\0';
	zassert_ok(sys_img_create(name, 10, digest, 0, &up));
	zassert_equal(strlen(up.filename), SYS_IMG_FILENAME_MAX);
}

ZTEST(system_image_store, test_create_size_limit)
{
	sha256_of((const uint8_t *)"x", 1, digest);
	zassert_equal(sys_img_create("a", MAX_IMAGE + 1U, digest, 0, &up), -EFBIG);
	zassert_equal(slot.erase_calls, 0);
	zassert_ok(sys_img_create("a", MAX_IMAGE, digest, 0, &up));
	zassert_equal(up.size_bytes, MAX_IMAGE);
	/* and it survives a reboot: the metadata's own size check agrees */
	reboot(0);
	zassert_ok(sys_img_get(up.id, 0, &up));
}

ZTEST(system_image_store, test_create_when_locked)
{
	sha256_of((const uint8_t *)"x", 1, digest);
	slot.locked = true;
	zassert_equal(sys_img_create("a", 100, digest, 0, &up), -EACCES);
	zassert_equal(slot.erase_calls, 0);
	zassert_false(exists(META));
	zassert_equal(sys_img_current(0, NULL), -ENOENT);
	/* too large outranks the lock */
	zassert_equal(sys_img_create("a", MAX_IMAGE + 1U, digest, 0, &up), -EFBIG);
	/* arguments outrank everything */
	zassert_equal(sys_img_create("", 100, digest, 0, &up), -EINVAL);
	slot.locked = false;
	zassert_ok(sys_img_create("a", 100, digest, 0, &up));
}

ZTEST(system_image_store, test_create_busy_then_failed_replaced)
{
	const struct fixture *bad = fixture("bad_magic");
	char first[SYS_IMG_ID_LEN + 1];

	create_for(bad, 0);
	strcpy(first, up.id);
	/* busy outranks too large and locked */
	zassert_equal(sys_img_create("b", MAX_IMAGE + 1U, digest, 0, NULL), -EBUSY);
	slot.locked = true;
	zassert_equal(sys_img_create("b", 100, digest, 0, NULL), -EBUSY);
	slot.locked = false;

	send(bad, 0, bad->len, CHUNK, 0);
	zassert_equal(sys_img_create("b", 100, digest, 0, NULL), -EBUSY);
	verify(0);
	zassert_equal(up.state, SYS_IMG_FAILED);

	create_for(fixture("valid"), 1);
	zassert_not_equal(strcmp(first, up.id), 0);
	zassert_equal(up.state, SYS_IMG_RECEIVING);
	zassert_equal(up.received_bytes, 0U);
	zassert_str_equal(up.error_code, "");
	zassert_equal(sys_img_get(first, 1, NULL), -ENOENT);
}

ZTEST(system_image_store, test_create_erase_failure)
{
	sha256_of((const uint8_t *)"x", 1, digest);
	slot.fail_erase_in = 1;
	zassert_equal(sys_img_create("a", 100, digest, 0, &up), -EIO);
	zassert_false(exists(META));
	zassert_equal(sys_img_current(0, NULL), -ENOENT);
	zassert_ok(sys_img_create("a", 100, digest, 0, &up));
}

ZTEST(system_image_store, test_failed_upload_replaced_even_if_erase_fails)
{
	const struct fixture *bad = fixture("bad_magic");

	upload(bad, 0);
	verify(0);
	slot.fail_erase_in = 1;
	zassert_equal(sys_img_create("a", 100, digest, 0, NULL), -EIO);
	/* the failed one is gone, and nothing new exists */
	zassert_equal(sys_img_current(0, NULL), -ENOENT);
	zassert_false(exists(META));
}

/* -- chunks -------------------------------------------------------------------------- */

ZTEST(system_image_store, test_chunk_accept_errors_in_order)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	zassert_equal(sys_img_chunk_accept("sysimg_0000000000000000", 0, fx->data, 10, 0), -ENOENT);
	zassert_equal(sys_img_chunk_accept(NULL, 0, fx->data, 10, 0), -ENOENT);
	slot.locked = true;
	zassert_equal(sys_img_chunk_accept(up.id, 7, NULL, 0, 0), -EACCES);
	slot.locked = false;
	zassert_equal(sys_img_chunk_accept(up.id, 1, fx->data, 10, 0), -ERANGE);
	zassert_equal(sys_img_chunk_accept(up.id, 0, fx->data, 0, 0), -ENODATA);
	zassert_equal(sys_img_chunk_accept(up.id, 0, NULL, 10, 0), -ENODATA);
	zassert_equal(sys_img_chunk_accept(up.id, 0, big, CHUNK + 1, 0), -E2BIG);
	zassert_equal(sys_img_chunk_accept(up.id, 0, fx->data, fx->len + 1, 0), -EOVERFLOW);
	zassert_equal(sys_img_chunk_accept(up.id, 0, fx->data, fx->len, 0), 0);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_true(up.chunk_pending);
	zassert_equal(up.received_bytes, 0U);
	/* pending outranks the offset */
	zassert_equal(sys_img_chunk_accept(up.id, 5, fx->data, 10, 0), -EBUSY);
	sys_img_chunk_discard(up.id);
	sys_img_chunk_discard("sysimg_0000000000000000");
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_false(up.chunk_pending);
	zassert_equal(sys_img_chunk_accept(up.id, 0, fx->data, CHUNK, 0), -EOVERFLOW);

	/* not receiving */
	send(fx, 0, fx->len, CHUNK, 0);
	zassert_equal(sys_img_chunk_accept(up.id, (uint32_t)fx->len, fx->data, 1, 0), -EOVERFLOW);
	verify(0);
	zassert_equal(sys_img_chunk_accept(up.id, 0, fx->data, 10, 0), -EINVAL);
	zassert_ok(sys_img_verify_begin(up.id, 0) == -EINVAL ? 0 : -1);
}

ZTEST(system_image_store, test_chunk_rejected_while_verifying)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	send(fx, 0, fx->len - 1U, CHUNK, 0);
	zassert_equal(sys_img_verify_begin(up.id, 0), -EINVAL);
	send(fx, fx->len - 1U, fx->len, CHUNK, 0);
	zassert_ok(sys_img_verify_begin(up.id, 0));
	/* receiving on the record, verifying in fact */
	zassert_equal(sys_img_chunk_accept(up.id, (uint32_t)fx->len, fx->data, 1, 0), -EINVAL);
}

ZTEST(system_image_store, test_commit_errors)
{
	create_for(fixture("valid"), 0);
	zassert_equal(sys_img_chunk_commit("sysimg_0000000000000000", 0), -ENOENT);
	zassert_equal(sys_img_chunk_commit(up.id, 0), -ENODATA);
}

ZTEST(system_image_store, test_upload_in_chunks_matches_the_file)
{
	const struct fixture *fx = fixture("valid_large");

	create_for(fx, 0);
	for (size_t off = 0; off < fx->len; off += CHUNK) {
		const size_t n = MIN((size_t)CHUNK, fx->len - off);

		zassert_ok(sys_img_chunk_accept(up.id, (uint32_t)off, &fx->data[off], n, 0));
		zassert_ok(sys_img_chunk_commit(up.id, 0));
		zassert_ok(sys_img_get(up.id, 0, &up));
		zassert_equal(up.received_bytes, off + n);
		zassert_false(up.chunk_pending);
		assert_slot(fx->data, 0, off + n);
	}
	verify(0);
	zassert_equal(up.state, SYS_IMG_READY, "%s", up.error_message);
}

ZTEST(system_image_store, test_uneven_chunks_over_a_dirty_slot)
{
	const struct fixture *fx = fixture("valid_large");

	/* an older image's bytes everywhere */
	slot_raw_fill(0, 0x00, 80000);
	create_for(fx, 0);
	send(fx, 0, 30001, 5000, 0);
	send(fx, 30001, 30004, 1, 0);
	send(fx, 30004, fx->len, 4096, 0);
	assert_slot(fx->data, 0, fx->len);
	verify(0);
	zassert_equal(up.state, SYS_IMG_READY, "%s", up.error_message);
}

ZTEST(system_image_store, test_mid_sector_chunk_keeps_accepted_bytes)
{
	const struct fixture *fx = fixture("valid_large");

	create_for(fx, 0);
	send(fx, 0, 5000, 5000, 0);
	/* 0 and 4096 start inside [0, 5000) */
	zassert_true(erased_sector(0));
	zassert_true(erased_sector(4096));

	slot_reset_knobs();
	send(fx, 5000, 10000, 5000, 0);
	/* only 8192 starts inside [5000, 10000) */
	zassert_equal(slot.erase_calls, 1);
	zassert_equal(slot.erase_off[0], 8192U);
	zassert_equal(slot.erase_len[0], 4096U);
	assert_slot(fx->data, 0, 10000);

	/* a chunk wholly inside one sector erases nothing */
	slot_reset_knobs();
	send(fx, 10000, 12000, 2000, 0);
	zassert_equal(slot.erase_calls, 0);
	/* one that ends exactly on a sector boundary erases nothing beyond it */
	slot_reset_knobs();
	send(fx, 12000, 12288, 288, 0);
	zassert_equal(slot.erase_calls, 0);
	slot_reset_knobs();
	send(fx, 12288, 16384, 4096, 0);
	zassert_equal(slot.erase_calls, 1);
	zassert_equal(slot.erase_off[0], 12288U);
	zassert_equal(slot.erase_len[0], 4096U);
	assert_slot(fx->data, 0, 16384);
}

/* -- stream_flash (CONFIG_SYSTEM_IMAGE_STORE_STREAM_FLASH) --------------------------- */

ZTEST(system_image_store, test_stream_upload_matches_the_file)
{
	const struct fixture *fx = fixture("valid_large");

	slot_stream(true);
	create_for(fx, 0);
	slot_reset_knobs();
	send(fx, 0, fx->len, 4096, 0);
	/* every chunk started on a sector: the store erased its sector through the
	 * platform (not STREAM_FLASH_ERASE), stream_flash wrote them all */
	zassert_equal(slot.erase_calls, DIV_ROUND_UP(fx->len, SECTOR));
	zassert_equal(slot.erase_off[1], SECTOR);
	zassert_equal(slot.erase_len[1], SECTOR);
	zassert_equal(slot.write_calls, 0);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, fx->len);
	assert_slot(fx->data, 0, fx->len);
	verify(0);
	zassert_equal(up.state, SYS_IMG_READY, "%s", up.error_message);
}

ZTEST(system_image_store, test_stream_erases_an_old_image_as_it_goes)
{
	const struct fixture *fx = fixture("valid_large");

	/* an older image's bytes everywhere: without the page erase the program
	 * would only clear bits and the read-back would fail */
	slot_raw_fill(0, 0x00, fx->len);
	slot_stream(true);
	create_for(fx, 0);
	send(fx, 0, fx->len, CHUNK, 0);
	assert_slot(fx->data, 0, fx->len);
	verify(0);
	zassert_equal(up.state, SYS_IMG_READY, "%s", up.error_message);
}

ZTEST(system_image_store, test_stream_unaligned_chunk_takes_the_platform_path)
{
	const struct fixture *fx = fixture("valid_large");

	slot_stream(true);
	create_for(fx, 0);
	slot_reset_knobs();
	send(fx, 0, 5000, 5000, 0);
	/* stream path: its two sectors erased up front, no platform write */
	zassert_equal(slot.erase_calls, 1);
	zassert_equal(slot.erase_off[0], 0U);
	zassert_equal(slot.erase_len[0], 2 * SECTOR);
	zassert_equal(slot.write_calls, 0);
	/* 5000 is inside a sector: the platform's erase of 8192 and write */
	send(fx, 5000, 10000, 5000, 0);
	zassert_equal(slot.erase_calls, 2);
	zassert_equal(slot.erase_off[1], 8192U);
	zassert_true(slot.write_calls > 0);
	/* aligned again: stream_flash */
	slot_reset_knobs();
	send(fx, 10000, 12288, 2288, 0);
	send(fx, 12288, fx->len, 4096, 0);
	zassert_equal(slot.write_calls, 1, "only the chunk at 10000 is unaligned");
	assert_slot(fx->data, 0, fx->len);
	verify(0);
	zassert_equal(up.state, SYS_IMG_READY, "%s", up.error_message);
}

ZTEST(system_image_store, test_resend_after_write_before_metadata)
{
	const struct fixture *fx = fixture("valid_large");

	create_for(fx, 0);
	send(fx, 0, CHUNK, CHUNK, 0);
	/* the next chunk reached flash (erased, half written), the metadata did not */
	zassert_ok(sys_img_chunk_accept(up.id, CHUNK, &fx->data[CHUNK], CHUNK, 0));
	slot_raw_write(CHUNK, &fx->data[CHUNK], CHUNK / 2);
	reboot(0);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, (uint32_t)CHUNK);
	zassert_false(up.chunk_pending);

	send(fx, CHUNK, fx->len, CHUNK, 0);
	assert_slot(fx->data, 0, fx->len);
	verify(0);
	zassert_equal(up.state, SYS_IMG_READY, "%s", up.error_message);
}

ZTEST(system_image_store, test_resend_mid_sector_over_partly_written_bytes)
{
	const struct fixture *fx = fixture("valid_large");

	create_for(fx, 0);
	send(fx, 0, 5000, 5000, 0);
	zassert_ok(sys_img_chunk_accept(up.id, 5000, &fx->data[5000], 5000, 0));
	/* part of it reached the (erased) rest of sector 4096 before the crash */
	slot_raw_write(5000, &fx->data[5000], 2000);
	reboot(0);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 5000U);

	slot_reset_knobs();
	send(fx, 5000, 10000, 5000, 0);
	/* programmed over, not repaired */
	zassert_false(erased_sector(4096));
	assert_slot(fx->data, 0, 10000);
}

ZTEST(system_image_store, test_repair_when_the_rest_of_a_sector_is_dirty)
{
	const struct fixture *fx = fixture("valid_large");

	create_for(fx, 0);
	send(fx, 0, 5000, 5000, 0);
	/* different bytes than the chunk after the accepted ones */
	slot_raw_fill(5000, 0x00, 1000);

	slot_reset_knobs();
	send(fx, 5000, 6000, 1000, 0);
	zassert_true(erased_sector(4096));
	assert_slot(fx->data, 0, 6000);
	/* the rest of the sector after the chunk is erased again */
	assert_erased(6000, 8192);

	/* also when the dirty byte is the chunk's last in the sector */
	send(fx, 6000, 8000, 2000, 0);
	slot_raw_fill(8191, 0x00, 1);
	slot_reset_knobs();
	send(fx, 8000, 9000, 1000, 0);
	zassert_true(erased_sector(4096));
	assert_slot(fx->data, 0, 9000);
}

ZTEST(system_image_store, test_head_check_stops_at_the_chunk_end)
{
	const struct fixture *fx = fixture("valid_large");

	create_for(fx, 0);
	send(fx, 0, 5000, 5000, 0);
	/* dirty, but after the next chunk's end in the same sector */
	slot_raw_fill(7000, 0x00, 1);
	slot_reset_knobs();
	send(fx, 5000, 6000, 1000, 0);
	zassert_false(erased_sector(4096));
	assert_slot(fx->data, 0, 6000);
	/* the chunk that reaches it repairs */
	slot_reset_knobs();
	send(fx, 6000, 8192, 2192, 0);
	zassert_true(erased_sector(4096));
	assert_slot(fx->data, 0, 8192);
}

/*
 * Make the metadata unwritable: a non-empty directory where the temporary file
 * goes. Opening it for writing fails, and so does unlinking it.
 */
static void block_tmp(void)
{
	const uint8_t x = 1;

	zassert_ok(fs_mkdir(TMP));
	write_file(TMP "/x", &x, 1);
}

static void unblock_tmp(void)
{
	zassert_ok(fs_unlink(TMP "/x"));
	zassert_ok(fs_unlink(TMP));
}

ZTEST(system_image_store, test_metadata_write_failures)
{
	const struct fixture *fx = fixture("valid");

	/* create */
	block_tmp();
	sha256_of(fx->data, fx->len, digest);
	zassert_equal(sys_img_create("a", (uint32_t)fx->len, digest, 0, &up), -EIO);
	zassert_equal(sys_img_current(0, NULL), -ENOENT);
	zassert_false(exists(META));
	unblock_tmp();

	/* commit: the bytes are in flash, received_bytes does not move */
	create_for(fx, 0);
	send(fx, 0, 1000, 1000, 0);
	block_tmp();
	zassert_ok(sys_img_chunk_accept(up.id, 1000, &fx->data[1000], 1000, 0));
	zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 1000U);
	zassert_false(up.chunk_pending);
	unblock_tmp();
	send(fx, 1000, fx->len, CHUNK, 0);

	/* verify: the outcome is not recorded, the upload is receiving */
	zassert_ok(sys_img_verify_begin(up.id, 0));
	block_tmp();
	zassert_equal(sys_img_verify(up.id, NULL, NULL, NULL, 0), -EIO);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_RECEIVING);
	zassert_false(up.has_image);
	unblock_tmp();
	verify(0);
	zassert_equal(up.state, SYS_IMG_READY);

	/* delete: a leftover temporary name does not stop it */
	block_tmp();
	zassert_ok(sys_img_delete(up.id));
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);
	unblock_tmp();

	/* delete: the record itself cannot be removed, the upload stays */
	upload(fx, 0);
	verify(0);
	{
		const uint8_t x = 1;

		zassert_ok(fs_unlink(META));
		zassert_ok(fs_mkdir(META));
		write_file(META "/x", &x, 1);
		zassert_equal(sys_img_delete(up.id), -EIO);
		zassert_ok(sys_img_get(up.id, 0, &up));
		zassert_equal(up.state, SYS_IMG_READY);
		zassert_ok(fs_unlink(META "/x"));
		zassert_ok(fs_unlink(META));
	}
	zassert_ok(sys_img_delete(up.id));

	/*
	 * A lost repair prefix whose rollback cannot be recorded: the upload in RAM
	 * no longer claims the lost bytes (a reboot would bring back the old record,
	 * and the final SHA-256 check would find the gap).
	 */
	create_for(fixture("valid_large"), 0);
	send(fixture("valid_large"), 0, 5000, 5000, 0);
	slot_raw_fill(5000, 0x00, 1);
	zassert_ok(sys_img_chunk_accept(up.id, 5000, &fixture("valid_large")->data[5000], 1000, 0));
	slot.fail_write_in = 1;
	block_tmp();
	zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO);
	unblock_tmp();
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 4096U);
	zassert_equal(sys_img_chunk_accept(up.id, 5000, &fixture("valid_large")->data[5000], 1000, 0),
		      -ERANGE);
}

ZTEST(system_image_store, test_create_while_rechecking_a_failed_upload)
{
	upload(fixture("bad_magic"), 0);
	verify(0);
	zassert_equal(up.state, SYS_IMG_FAILED);
	zassert_ok(sys_img_verify_begin(up.id, 0));
	zassert_equal(sys_img_create("b", 100, digest, 0, NULL), -EBUSY);
	zassert_equal(sys_img_delete(up.id), -EBUSY);
}

ZTEST(system_image_store, test_create_busy_while_ready)
{
	upload(fixture("valid"), 0);
	verify(0);
	zassert_equal(up.state, SYS_IMG_READY);
	zassert_equal(sys_img_create("b", 100, digest, 0, NULL), -EBUSY);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_READY);
}

ZTEST(system_image_store, test_offset_below_received_is_a_mismatch)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	send(fx, 0, 1000, 1000, 0);
	zassert_equal(sys_img_chunk_accept(up.id, 0, fx->data, 10, 0), -ERANGE);
	zassert_equal(sys_img_chunk_accept(up.id, 999, &fx->data[999], 10, 0), -ERANGE);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_false(up.chunk_pending);
}

ZTEST(system_image_store, test_accepted_chunk_is_activity)
{
	const struct fixture *fx = fixture("valid");
	const int64_t accepted = EXPIRE_MS / 2;

	create_for(fx, 0);
	zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, 10, accepted));
	sys_img_chunk_discard(up.id);
	zassert_ok(sys_img_get(up.id, accepted + EXPIRE_MS - 1, NULL));
	zassert_equal(sys_img_get(up.id, accepted + EXPIRE_MS, NULL), -ENOENT);
}

ZTEST(system_image_store, test_reported_write_error_fails_the_chunk)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, 1000, 0));
	slot.write_fail_after_in = 1;
	zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 0U);
}

ZTEST(system_image_store, test_programmable_bits_are_not_repaired)
{
	const struct fixture *fx = fixture("valid_large");
	uint8_t b = fx->data[5000] | 0x0F;

	create_for(fx, 0);
	send(fx, 0, 5000, 5000, 0);
	/* bits the chunk's byte still clears: programmable */
	slot_raw_write(5000, &b, 1);
	slot_reset_knobs();
	send(fx, 5000, 6000, 1000, 0);
	zassert_false(erased_sector(4096));
	assert_slot(fx->data, 0, 6000);
}

ZTEST(system_image_store, test_readback_mismatch_fails_the_chunk)
{
	const struct fixture *fx = fixture("valid_large");

	create_for(fx, 0);
	zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, CHUNK, 0));
	slot.corrupt_read_in = 1;
	zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 0U);
	zassert_false(up.chunk_pending);
	send(fx, 0, fx->len, CHUNK, 0);
	assert_slot(fx->data, 0, fx->len);
}

ZTEST(system_image_store, test_readback_checks_every_piece)
{
	const struct fixture *fx = fixture("valid_large");

	/* An aligned 16 KiB chunk: the read-back is its only reads, in 4 KiB pieces. */
	create_for(fx, 0);
	for (int piece = 1; piece <= CHUNK / CONFIG_SYSTEM_IMAGE_STORE_READ_BUF; piece++) {
		slot_reset_knobs();
		zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, CHUNK, 0));
		slot.corrupt_read_in = piece;
		zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO, "piece %d", piece);
		slot_reset_knobs();
		zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, CHUNK, 0));
		slot.fail_read_in = piece;
		zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO, "piece %d", piece);
	}
	slot_reset_knobs();
	send(fx, 0, CHUNK, CHUNK, 0);
	assert_slot(fx->data, 0, CHUNK);
}

ZTEST(system_image_store, test_flash_failures_fail_the_chunk)
{
	const struct fixture *fx = fixture("valid_large");

	create_for(fx, 0);
	/* erase */
	zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, 5000, 0));
	slot.fail_erase_in = 1;
	zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO);
	/* write */
	zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, 5000, 0));
	slot.fail_write_in = 1;
	zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 0U);
	send(fx, 0, 5000, 5000, 0);

	/* the head check's read */
	zassert_ok(sys_img_chunk_accept(up.id, 5000, &fx->data[5000], 1000, 0));
	slot.fail_read_in = 1;
	zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO);

	/*
	 * Each step of a repair. Before the erase nothing is lost; from the erase on
	 * [4096, 5000) may be, and received_bytes drops to 4096 durably - the client's
	 * resend at 5000 then meets offset_mismatch and it sends 4096 again.
	 */
	for (int step = 0; step < 3; step++) {
		slot_reset_knobs();
		zassert_ok(sys_img_get(up.id, 0, &up));
		send(fx, up.received_bytes, 5000, 5000, 0);
		slot_raw_fill(5000, 0x00, 1);
		zassert_ok(sys_img_chunk_accept(up.id, 5000, &fx->data[5000], 1000, 0));
		if (step == 0) {
			slot.fail_read_in = 2; /* head check, then the prefix read */
		} else if (step == 1) {
			slot.fail_erase_in = 1;
		} else {
			slot.fail_write_in = 1;
		}
		zassert_equal(sys_img_chunk_commit(up.id, 0), -EIO, "step %d", step);
		zassert_ok(sys_img_get(up.id, 0, &up));
		zassert_false(up.chunk_pending);
		zassert_equal(up.received_bytes, step == 0 ? 5000U : 4096U, "step %d", step);
		reboot(0);
		zassert_ok(sys_img_get(up.id, 0, &up));
		zassert_equal(up.received_bytes, step == 0 ? 5000U : 4096U, "step %d", step);
		if (step > 0) {
			zassert_equal(sys_img_chunk_accept(up.id, 5000, &fx->data[5000], 1000, 0),
				      -ERANGE);
		}
	}
	slot_reset_knobs();
	send(fx, 4096, 6000, 1000, 0);
	assert_slot(fx->data, 0, 6000);
}

ZTEST(system_image_store, test_locked_slot_refuses_chunks)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	slot.locked = true;
	zassert_equal(sys_img_chunk_accept(up.id, 0, fx->data, 10, 0), -EACCES);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_false(up.chunk_pending);
}

/* -- recovery ------------------------------------------------------------------------- */

ZTEST(system_image_store, test_reboot_keeps_a_receiving_upload)
{
	const struct fixture *fx = fixture("valid_large");
	char id[SYS_IMG_ID_LEN + 1];

	create_for(fx, 0);
	strcpy(id, up.id);
	send(fx, 0, 2U * CHUNK, CHUNK, 0);
	zassert_ok(sys_img_set_active_job(up.id, "job_1"));
	zassert_ok(sys_img_chunk_accept(up.id, 2U * CHUNK, &fx->data[2U * CHUNK], 10, 0));
	reboot(100);

	zassert_ok(sys_img_get(id, 100, &up));
	zassert_equal(up.received_bytes, 2U * CHUNK);
	zassert_equal(up.state, SYS_IMG_RECEIVING);
	zassert_false(up.chunk_pending);
	zassert_str_equal(up.active_job_id, "");
	send(fx, 2U * CHUNK, fx->len, CHUNK, 100);
	verify(100);
	zassert_equal(up.state, SYS_IMG_READY, "%s", up.error_message);
}

ZTEST(system_image_store, test_reboot_keeps_a_ready_upload_the_slot_still_holds)
{
	const struct fixture *fx = fixture("valid");
	struct sys_img_image img;

	upload(fx, 0);
	verify(0);
	reboot(0);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_READY);
	zassert_str_equal(up.version, "1.2.3+4");
	zassert_ok(sys_img_image_info(up.id, &img));
	zassert_mem_equal(img.image_hash, &fx->data[fx->len - 32], 32);

	/* with a protected TLV area in front of the SHA-256 TLV too */
	zassert_ok(sys_img_delete(up.id));
	upload(fixture("valid_prot_tlv"), 0);
	verify(0);
	zassert_equal(up.state, SYS_IMG_READY, "%s", up.error_message);
	reboot(0);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_READY);
}

ZTEST(system_image_store, test_reboot_forgets_a_ready_upload_the_slot_lost)
{
	const struct fixture *fx = fixture("valid");
	const struct fixture *other = fixture("wrong_sp");

	/* the slot erased */
	upload(fx, 0);
	verify(0);
	slot_erase_all();
	reboot(0);
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);
	zassert_false(exists(META));

	/* another image of the same size: a different SHA-256 TLV */
	upload(fx, 0);
	verify(0);
	zassert_equal(fx->len, other->len);
	slot_erase_all();
	slot_raw_write(0, other->data, other->len);
	reboot(0);
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);

	/* an image of another size */
	upload(fx, 0);
	verify(0);
	slot_erase_all();
	slot_raw_write(0, fixture("valid_prot_tlv")->data, fixture("valid_prot_tlv")->len);
	reboot(0);
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);

	/* the same image with its SHA-256 TLV retyped */
	upload(fx, 0);
	verify(0);
	slot_erase_all();
	slot_raw_write(0, fixture("no_sha_tlv")->data, fx->len);
	reboot(0);
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);

	/* a TLV info whose total points elsewhere */
	upload(fx, 0);
	verify(0);
	slot_erase_all();
	slot_raw_write(0, fixture("trailing_bytes")->data, fx->len);
	{
		uint8_t tot[2];

		sys_put_le16(56, tot);
		slot_erase_all();
		slot_raw_write(0, fx->data, fx->len - 40U);
		static uint8_t tail[40];

		memcpy(tail, &fx->data[fx->len - 40U], 40);
		memcpy(&tail[2], tot, 2);
		slot_raw_write((uint32_t)fx->len - 40U, tail, 40);
	}
	reboot(0);
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);

	/* a header whose image size runs past the slot: forgotten, not unreadable */
	upload(fx, 0);
	verify(0);
	slot_erase_all();
	{
		uint8_t hdr[32] = {0};

		sys_put_le32(MCUBOOT_IMAGE_MAGIC, hdr);
		sys_put_le16(TEST_HDR, &hdr[8]);
		sys_put_le32(0x7FFFFFF0U, &hdr[12]);
		slot_raw_write(0, hdr, sizeof(hdr));
	}
	reboot(0);
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);

	/* the same image with its TLV info magic damaged in the slot */
	upload(fx, 0);
	verify(0);
	{
		const uint8_t damaged = (uint8_t)(MCUBOOT_TLV_INFO_MAGIC & 0xFEU);

		slot_raw_write((uint32_t)fx->len - 40U, &damaged, 1);
	}
	reboot(0);
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);
}

ZTEST(system_image_store, test_reboot_keeps_a_failed_upload)
{
	const struct fixture *bad = fixture("wrong_reset");

	upload(bad, 0);
	verify(0);
	zassert_equal(up.state, SYS_IMG_FAILED);
	slot_erase_all();
	reboot(0);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_FAILED);
	zassert_str_equal(up.error_code, "unsupported_target");
	zassert_not_null(strstr(up.error_message, "reset vector"));
}

static void edit_byte_flip(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	m[META_FILENAME + 1] ^= 0x01;
}

ZTEST(system_image_store, test_reboot_forgets_corrupt_metadata)
{
	static uint8_t m[2048];
	size_t len;

	/* a flipped bit: the CRC disagrees */
	create_for(fixture("valid"), 0);
	len = read_file(META, m, sizeof(m));
	m[META_FILENAME] ^= 0x01;
	write_file(META, m, len);
	reboot(0);
	zassert_equal(sys_img_current(0, NULL), -ENOENT);
	zassert_false(exists(META));

	/* short */
	create_for(fixture("valid"), 0);
	len = read_file(META, m, sizeof(m));
	write_file(META, m, len - 1U);
	reboot(0);
	zassert_equal(sys_img_current(0, NULL), -ENOENT);
	zassert_false(exists(META));

	/* long */
	create_for(fixture("valid"), 0);
	len = read_file(META, m, sizeof(m));
	m[len] = 0;
	write_file(META, m, len + 1U);
	reboot(0);
	zassert_equal(sys_img_current(0, NULL), -ENOENT);

	/* a CRC recomputed over a legal change is fine */
	create_for(fixture("valid"), 0);
	tamper_meta(edit_byte_flip);
	reboot(0);
	zassert_ok(sys_img_current(0, &up));
}

static void edit_prefix(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	m[META_ID] = 'X';
}
static void edit_id_short(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	m[META_ID + SYS_IMG_ID_LEN - 1] = '\0';
}
static void edit_id_unterminated(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	m[META_ID + SYS_IMG_ID_LEN] = 'x';
}
static void edit_state_verifying(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	m[META_STATE] = SYS_IMG_VERIFYING;
}
static void edit_state_unknown(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	m[META_STATE] = 9;
}
static void edit_size_zero(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	sys_put_le32(0, &m[META_SIZE]);
}
static void edit_size_too_big(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	sys_put_le32(MAX_IMAGE + 1U, &m[META_SIZE]);
}
static void edit_received_past_size(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	sys_put_le32(sys_get_le32(&m[META_SIZE]) + 1U, &m[META_RECEIVED]);
}
static void edit_filename_empty(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	m[META_FILENAME] = '\0';
}
static void edit_filename_unterminated(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	memset(&m[META_FILENAME], 'f', SYS_IMG_FILENAME_MAX + 1U);
}
static void edit_magic(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	m[0] ^= 0xFF;
}
static void edit_version(uint8_t *m, size_t len)
{
	ARG_UNUSED(len);
	m[4] = 2;
}
static void edit_strings_unterminated(uint8_t *m, size_t len)
{
	/* the last 128 + 32 + ... bytes before the CRC: error message, code, and the
	 * image hash and version text further back; fill the tail with non-NULs */
	memset(&m[len - 4U - (SYS_IMG_ERROR_MESSAGE_MAX + 1U)], 'e', SYS_IMG_ERROR_MESSAGE_MAX + 1U);
}
static void edit_code_unterminated(uint8_t *m, size_t len)
{
	const size_t at = len - 4U - (SYS_IMG_ERROR_MESSAGE_MAX + 1U) - (SYS_IMG_ERROR_CODE_MAX + 1U);

	memset(&m[at], 'c', SYS_IMG_ERROR_CODE_MAX + 1U);
}
static void edit_version_unterminated(uint8_t *m, size_t len)
{
	/* version text, then major, minor, revision (2), build (4), image hash (32) */
	const size_t at = len - 4U - (SYS_IMG_ERROR_MESSAGE_MAX + 1U) -
			  (SYS_IMG_ERROR_CODE_MAX + 1U) - 32U - 8U - (MCUBOOT_IMAGE_VERSION_MAX + 1U);

	memset(&m[at], 'v', MCUBOOT_IMAGE_VERSION_MAX + 1U);
}

ZTEST(system_image_store, test_reboot_forgets_implausible_metadata)
{
	static void (*const edits[])(uint8_t *, size_t) = {
		edit_prefix,         edit_id_short,          edit_id_unterminated,
		edit_state_verifying, edit_state_unknown,     edit_size_zero,
		edit_size_too_big,   edit_received_past_size, edit_filename_empty,
		edit_filename_unterminated, edit_magic,      edit_version,
		edit_strings_unterminated, edit_code_unterminated, edit_version_unterminated,
	};

	for (size_t i = 0; i < ARRAY_SIZE(edits); i++) {
		create_for(fixture("valid"), 0);
		tamper_meta(edits[i]);
		reboot(0);
		zassert_equal(sys_img_current(0, NULL), -ENOENT, "edit %zu kept", i);
		zassert_false(exists(META), "edit %zu", i);
	}
}

ZTEST(system_image_store, test_reboot_removes_a_leftover_temporary)
{
	const uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};

	create_for(fixture("valid"), 0);
	write_file(TMP, junk, sizeof(junk));
	reboot(0);
	zassert_false(exists(TMP));
	zassert_ok(sys_img_get(up.id, 0, NULL));

	/* also without an upload */
	zassert_ok(sys_img_delete(up.id));
	write_file(TMP, junk, sizeof(junk));
	reboot(0);
	zassert_false(exists(TMP));
}

ZTEST(system_image_store, test_init_creates_the_directory)
{
	zassert_ok(sys_img_init(TEST_MNT "/elsewhere", &platform, 0));
	struct fs_dirent e;

	zassert_ok(fs_stat(TEST_MNT "/elsewhere", &e));
	zassert_equal(e.type, FS_DIR_ENTRY_DIR);
	sha256_of((const uint8_t *)"x", 1, digest);
	zassert_ok(sys_img_create("a", 10, digest, 0, NULL));
	zassert_ok(fs_stat(TEST_MNT "/elsewhere/sysimg.meta", &e));
	zassert_equal(sys_img_init(TEST_MNT "/no/such/parent", &platform, 0), -EIO);
}

/* -- verify --------------------------------------------------------------------------- */

struct progress {
	int calls;
	uint32_t done;
	uint32_t total;
	int cancel_at;
};

static void on_progress(void *ctx, uint32_t done, uint32_t total)
{
	struct progress *p = ctx;

	zassert_true(done > p->done, "progress went backwards");
	p->calls++;
	p->done = done;
	p->total = total;
}

static bool on_cancel(void *ctx)
{
	struct progress *p = ctx;

	return p->cancel_at >= 0 && p->calls >= p->cancel_at;
}

ZTEST(system_image_store, test_verify_begin_errors)
{
	const struct fixture *fx = fixture("valid");

	zassert_equal(sys_img_verify_begin("sysimg_0000000000000000", 0), -ENOENT);
	create_for(fx, 0);
	zassert_equal(sys_img_verify_begin(up.id, 0), -EINVAL);
	send(fx, 0, fx->len - 10U, CHUNK, 0);
	zassert_ok(sys_img_chunk_accept(up.id, (uint32_t)fx->len - 10U, &fx->data[fx->len - 10U], 10, 0));
	/* pending outranks incomplete */
	zassert_equal(sys_img_verify_begin(up.id, 0), -EBUSY);
	zassert_ok(sys_img_chunk_commit(up.id, 0));
	zassert_ok(sys_img_verify_begin(up.id, 0));
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_VERIFYING);
	zassert_equal(sys_img_verify_begin(up.id, 0), -EINVAL);
	zassert_ok(sys_img_verify(up.id, NULL, NULL, NULL, 0));
	zassert_equal(sys_img_verify_begin(up.id, 0), -EINVAL);
	zassert_equal(sys_img_verify(up.id, NULL, NULL, NULL, 0), -ENOENT);
	zassert_ok(sys_img_set_in_use(up.id, true));
	zassert_equal(sys_img_verify_begin(up.id, 0), -EBUSY);
}

ZTEST(system_image_store, test_verify_accepts_and_reports)
{
	const struct fixture *fx = fixture("valid_large");
	struct progress p = {.cancel_at = -1};
	struct sys_img_image img;

	upload(fx, 0);
	zassert_ok(sys_img_verify_begin(up.id, 0));
	zassert_ok(sys_img_verify(up.id, on_cancel, on_progress, &p, 3));
	zassert_ok(sys_img_get(up.id, 3, &up));
	zassert_equal(up.state, SYS_IMG_READY, "%s", up.error_message);
	zassert_true(up.has_image);
	zassert_str_equal(up.version, "2.0.1+77");
	zassert_mem_equal(up.image_hash, &fx->data[fx->len - 32], 32);
	zassert_str_equal(up.error_code, "");
	zassert_str_equal(up.error_message, "");
	zassert_true(p.calls > 1);
	zassert_equal(p.done, fx->len);
	zassert_equal(p.total, fx->len);

	zassert_ok(sys_img_image_info(up.id, &img));
	zassert_str_equal(img.version, "2.0.1+77");
	zassert_equal(img.major, 2);
	zassert_equal(img.minor, 0);
	zassert_equal(img.revision, 1);
	zassert_equal(img.build, 77);
	zassert_equal(img.size_bytes, fx->len);
	zassert_mem_equal(img.image_hash, up.image_hash, 32);
	zassert_ok(sys_img_image_info(up.id, NULL));

	/* all of it survives a reboot */
	reboot(3);
	memset(&img, 0, sizeof(img));
	zassert_ok(sys_img_image_info(up.id, &img));
	zassert_equal(img.build, 77);
	zassert_equal(img.revision, 1);
	zassert_str_equal(img.version, "2.0.1+77");
}

ZTEST(system_image_store, test_verify_rejects_the_corpus)
{
	for (size_t i = 0; i < corpus_count; i++) {
		const struct fixture *fx = &corpus[i];

		if (fx->code == NULL) {
			continue;
		}
		upload(fx, 0);
		verify(0);
		zassert_equal(up.state, SYS_IMG_FAILED, "%s", fx->name);
		zassert_false(up.has_image, "%s", fx->name);
		zassert_str_equal(up.version, "", "%s", fx->name);
		zassert_str_equal(up.error_code, fx->code, "%s", fx->name);
		zassert_not_null(strstr(up.error_message, fx->message), "%s: %s", fx->name,
				 up.error_message);
		zassert_equal(sys_img_image_info(up.id, NULL), -EINVAL, "%s", fx->name);
	}
	upload(&esp32_merged, 0);
	verify(0);
	zassert_str_equal(up.error_code, "invalid_image");
}

ZTEST(system_image_store, test_declared_digest_outranks)
{
	const struct fixture *fx = fixture("valid");
	uint8_t wrong[32];

	memset(wrong, 0xAB, sizeof(wrong));
	zassert_ok(sys_img_create("a", (uint32_t)fx->len, wrong, 0, &up));
	send(fx, 0, fx->len, CHUNK, 0);
	verify(0);
	zassert_equal(up.state, SYS_IMG_FAILED);
	zassert_str_equal(up.error_code, "invalid_image");
	zassert_not_null(strstr(up.error_message, "given when the upload was created"));

	fx = fixture("wrong_sp");
	zassert_ok(sys_img_create("a", (uint32_t)fx->len, wrong, 0, &up));
	send(fx, 0, fx->len, CHUNK, 0);
	verify(0);
	zassert_str_equal(up.error_code, "invalid_image");
	zassert_not_null(strstr(up.error_message, "given when the upload was created"));
}

ZTEST(system_image_store, test_verify_cancelled)
{
	const struct fixture *fx = fixture("valid_large");
	struct progress p = {.cancel_at = 3};

	upload(fx, 0);
	zassert_ok(sys_img_verify_begin(up.id, 0));
	zassert_equal(sys_img_verify(up.id, on_cancel, on_progress, &p, 0), -ECANCELED);
	zassert_equal(p.calls, 3);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_RECEIVING);
	zassert_equal(up.received_bytes, fx->len);

	/* a failed upload checked again and cancelled is receiving, durably */
	zassert_ok(sys_img_delete(up.id));
	upload(fixture("bad_magic"), 0);
	verify(0);
	zassert_equal(up.state, SYS_IMG_FAILED);
	p = (struct progress){.cancel_at = 0};
	zassert_ok(sys_img_verify_begin(up.id, 0));
	zassert_equal(sys_img_verify(up.id, on_cancel, NULL, &p, 0), -ECANCELED);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_RECEIVING);
	zassert_str_equal(up.error_code, "");
	reboot(0);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_RECEIVING);
	zassert_str_equal(up.error_code, "");
	zassert_ok(sys_img_verify_begin(up.id, 0));
}

ZTEST(system_image_store, test_verify_read_failure)
{
	const struct fixture *fx = fixture("valid_large");

	upload(fx, 0);
	zassert_ok(sys_img_verify_begin(up.id, 0));
	slot.fail_read_in = 5;
	zassert_equal(sys_img_verify(up.id, NULL, NULL, NULL, 0), -EIO);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_RECEIVING);
	zassert_ok(sys_img_verify_begin(up.id, 0));
	zassert_ok(sys_img_verify(up.id, NULL, NULL, NULL, 0));

	/* a failed upload whose re-check cannot read stays failed */
	zassert_ok(sys_img_delete(up.id));
	upload(fixture("bad_magic"), 0);
	verify(0);
	zassert_ok(sys_img_verify_begin(up.id, 0));
	slot.fail_read_in = 1;
	zassert_equal(sys_img_verify(up.id, NULL, NULL, NULL, 0), -EIO);
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(up.state, SYS_IMG_FAILED);
}

ZTEST(system_image_store, test_verify_unclaimed)
{
	upload(fixture("valid"), 0);
	zassert_equal(sys_img_verify(up.id, NULL, NULL, NULL, 0), -ENOENT);
	zassert_equal(sys_img_verify("sysimg_0000000000000000", NULL, NULL, NULL, 0), -ENOENT);
}

/* -- delete, in use, jobs, info ---------------------------------------------------- */

ZTEST(system_image_store, test_delete)
{
	const struct fixture *fx = fixture("valid");

	zassert_equal(sys_img_delete("sysimg_0000000000000000"), -ENOENT);
	upload(fx, 0);
	zassert_ok(sys_img_verify_begin(up.id, 0));
	zassert_equal(sys_img_delete(up.id), -EBUSY);
	zassert_ok(sys_img_verify(up.id, NULL, NULL, NULL, 0));
	zassert_ok(sys_img_set_in_use(up.id, true));
	zassert_equal(sys_img_delete(up.id), -EBUSY);
	zassert_ok(sys_img_set_in_use(up.id, false));

	slot_reset_knobs();
	zassert_ok(sys_img_delete(up.id));
	zassert_equal(sys_img_get(up.id, 0, NULL), -ENOENT);
	zassert_false(exists(META));
	/* the slot is left as it was */
	zassert_equal(slot.erase_calls, 0);
	assert_slot(fx->data, 0, fx->len);
	zassert_equal(sys_img_delete(up.id), -ENOENT);

	/* pending blocks it */
	create_for(fx, 0);
	zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, 10, 0));
	zassert_equal(sys_img_delete(up.id), -EBUSY);
}

ZTEST(system_image_store, test_in_use_and_active_job)
{
	const struct fixture *fx = fixture("valid");

	zassert_equal(sys_img_set_in_use("sysimg_0000000000000000", true), -ENOENT);
	zassert_equal(sys_img_set_active_job("sysimg_0000000000000000", "j"), -ENOENT);
	create_for(fx, 0);
	zassert_equal(sys_img_set_in_use(up.id, true), -EINVAL);
	zassert_ok(sys_img_set_in_use(up.id, false));
	zassert_ok(sys_img_set_active_job(up.id, "job_00000042"));
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_str_equal(up.active_job_id, "job_00000042");
	zassert_ok(sys_img_set_active_job(up.id, NULL));
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_str_equal(up.active_job_id, "");
	zassert_ok(sys_img_set_active_job(up.id, "job_1"));
	zassert_ok(sys_img_set_active_job(up.id, ""));
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_str_equal(up.active_job_id, "");
	/* too long is cut to what fits */
	zassert_ok(sys_img_set_active_job(up.id, "job_0123456789012345678901234567890123"));
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_equal(strlen(up.active_job_id), SYS_IMG_JOB_ID_MAX);

	send(fx, 0, fx->len, CHUNK, 0);
	verify(0);
	zassert_ok(sys_img_set_in_use(up.id, true));
	zassert_ok(sys_img_get(up.id, 0, &up));
	zassert_true(up.in_use);
	zassert_equal(sys_img_create("b", 10, digest, 0, NULL), -EBUSY);
	zassert_ok(sys_img_image_info(up.id, NULL));
}

ZTEST(system_image_store, test_image_info_errors)
{
	const struct fixture *fx = fixture("valid");

	zassert_equal(sys_img_image_info("sysimg_0000000000000000", NULL), -ENOENT);
	create_for(fx, 0);
	zassert_equal(sys_img_image_info(up.id, NULL), -EINVAL);
	send(fx, 0, fx->len, CHUNK, 0);
	zassert_equal(sys_img_image_info(up.id, NULL), -EINVAL);
	verify(0);
	zassert_ok(sys_img_image_info(up.id, NULL));
	/* a ready upload being checked again is not ready for the updater */
	zassert_equal(sys_img_verify_begin(up.id, 0), -EINVAL);
}

/* -- expiry --------------------------------------------------------------------------- */

ZTEST(system_image_store, test_expiry)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 1000);
	/* reading is not activity */
	zassert_ok(sys_img_get(up.id, 1000 + EXPIRE_MS - 1, NULL));
	zassert_ok(sys_img_current(1000 + EXPIRE_MS - 1, NULL));
	zassert_equal(sys_img_get(up.id, 1000 + EXPIRE_MS, NULL), -ENOENT);
	zassert_false(exists(META));

	/* a chunk is activity; tick expires */
	create_for(fx, 0);
	zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, 10, 500));
	zassert_ok(sys_img_chunk_commit(up.id, 600));
	sys_img_tick(600 + EXPIRE_MS - 1);
	zassert_ok(sys_img_get(up.id, 600 + EXPIRE_MS - 1, NULL));
	zassert_true(exists(META));
	sys_img_tick(600 + EXPIRE_MS);
	/* the tick alone removed it, before any other call looks */
	zassert_false(exists(META));
	zassert_equal(sys_img_current(600 + EXPIRE_MS, NULL), -ENOENT);

	/* accept alone is activity */
	create_for(fx, 0);
	zassert_ok(sys_img_chunk_accept(up.id, 0, fx->data, 10, 700));
	zassert_ok(sys_img_get(up.id, 700 + EXPIRE_MS - 1, NULL));
	/* pending blocks it */
	zassert_ok(sys_img_get(up.id, 700 + 3 * EXPIRE_MS, NULL));
	zassert_ok(sys_img_chunk_commit(up.id, 700));
	/* the commit's time counts */
	zassert_equal(sys_img_get(up.id, 700 + EXPIRE_MS, NULL), -ENOENT);

	/* in use blocks it; create and chunks expire it first */
	upload(fx, 0);
	verify(0);
	zassert_ok(sys_img_set_in_use(up.id, true));
	zassert_ok(sys_img_get(up.id, 5 * EXPIRE_MS, NULL));
	zassert_ok(sys_img_set_in_use(up.id, false));
	zassert_ok(sys_img_create("b", 10, digest, 5 * EXPIRE_MS, &up));

	/* verifying blocks it */
	zassert_ok(sys_img_delete(up.id));
	upload(fixture("bad_magic"), 0);
	verify(10);
	zassert_ok(sys_img_verify_begin(up.id, 20));
	zassert_ok(sys_img_get(up.id, 20 + 2 * EXPIRE_MS, NULL));
	zassert_ok(sys_img_verify(up.id, NULL, NULL, NULL, 30));
	zassert_ok(sys_img_get(up.id, 30 + EXPIRE_MS - 1, NULL));
	zassert_equal(sys_img_chunk_accept(up.id, 0, fx->data, 1, 30 + EXPIRE_MS), -ENOENT);

	/* the period restarts at init */
	create_for(fx, 0);
	reboot(10 * EXPIRE_MS);
	zassert_ok(sys_img_get(up.id, 10 * EXPIRE_MS + EXPIRE_MS - 1, NULL));
	zassert_equal(sys_img_verify_begin(up.id, 11 * EXPIRE_MS), -ENOENT);
}

ZTEST(system_image_store, test_state_strings)
{
	zassert_str_equal(sys_img_state_str(SYS_IMG_RECEIVING), "receiving");
	zassert_str_equal(sys_img_state_str(SYS_IMG_VERIFYING), "verifying");
	zassert_str_equal(sys_img_state_str(SYS_IMG_READY), "ready");
	zassert_str_equal(sys_img_state_str(SYS_IMG_FAILED), "failed");
	zassert_str_equal(sys_img_state_str((enum sys_img_state)9), "unknown");
}
