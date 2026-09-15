/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * firmware-store on LittleFS over the sim flash: the upload lifecycle, the
 * errno contract, recovery after a "reboot" (remount and init), leftovers,
 * space checks, expiry and the image reader.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <firmware_store/firmware_store.h>

#include "common.h"

#define EXPIRE_MS ((int64_t)CONFIG_FIRMWARE_STORE_EXPIRE_SECONDS * 1000)
#define CHUNK     CONFIG_FIRMWARE_STORE_CHUNK_MAX
#define BIN       TEST_DIR "/upload.bin"
#define META      TEST_DIR "/upload.meta"

static struct fw_upload up;
static uint8_t digest[32];
static uint8_t big[CHUNK + 1];

/* -- helpers ------------------------------------------------------------------------ */

static bool exists(const char *path)
{
	struct fs_dirent e;

	return fs_stat(path, &e) == 0;
}

static size_t size_of(const char *path)
{
	struct fs_dirent e;

	zassert_ok(fs_stat(path, &e), "%s", path);
	return e.size;
}

static void write_file(const char *path, const uint8_t *data, size_t len, bool append)
{
	struct fs_file_t f;

	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | (append ? FS_O_APPEND : FS_O_TRUNC)));
	zassert_equal(fs_write(&f, data, len), (ssize_t)len);
	zassert_ok(fs_close(&f));
}

static void truncate_to(const char *path, size_t len)
{
	struct fs_file_t f;

	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, path, FS_O_RDWR));
	zassert_ok(fs_truncate(&f, len));
	zassert_ok(fs_close(&f));
}

static void reboot(int64_t now)
{
	fw_store_image_close();
	volume_remount();
	zassert_ok(fw_store_init(TEST_DIR, now));
}

static void create_for(const struct fixture *fx, int64_t now)
{
	sha256_of(fx->data, fx->len, digest);
	zassert_ok(fw_store_create("cedar-c6.bin", (uint32_t)fx->len, digest, now, &up));
}

static void send(const struct fixture *fx, size_t from, size_t to, int64_t now)
{
	for (size_t off = from; off < to; off += CHUNK) {
		const size_t n = MIN((size_t)CHUNK, to - off);

		zassert_ok(fw_store_chunk_accept(up.id, (uint32_t)off, &fx->data[off], n, now),
			   "offset %zu", off);
		zassert_ok(fw_store_chunk_commit(up.id, now), "offset %zu", off);
	}
}

static void upload(const struct fixture *fx, int64_t now)
{
	create_for(fx, now);
	send(fx, 0, fx->len, now);
}

static void verify(int64_t now)
{
	zassert_ok(fw_store_verify_begin(up.id, now));
	zassert_ok(fw_store_verify(up.id, NULL, NULL, NULL, now));
	zassert_ok(fw_store_get(up.id, now, &up));
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	fw_store_image_close();
	volume_fresh();
	zassert_ok(fw_store_init(TEST_DIR, 0));
	memset(&up, 0, sizeof(up));
}

ZTEST_SUITE(firmware_store, NULL, NULL, before, NULL, NULL);

/* -- create and get --------------------------------------------------------------- */

ZTEST(firmware_store, test_create_starts_receiving)
{
	const struct fixture *fx = fixture("valid");
	struct fw_upload got;

	create_for(fx, 5);
	zassert_equal(strlen(up.id), FW_STORE_ID_LEN);
	zassert_mem_equal(up.id, "upload_", 7);
	zassert_str_equal(up.filename, "cedar-c6.bin");
	zassert_equal(up.size_bytes, fx->len);
	zassert_equal(up.received_bytes, 0U);
	zassert_mem_equal(up.sha256, digest, sizeof(digest));
	zassert_equal(up.state, FW_UPLOAD_RECEIVING);
	zassert_false(up.has_image);
	zassert_false(up.in_use);
	zassert_false(up.chunk_pending);
	zassert_str_equal(up.active_job_id, "");
	zassert_str_equal(up.error_code, "");
	zassert_true(exists(META));
	zassert_false(exists(BIN));

	zassert_ok(fw_store_get(up.id, 6, &got));
	zassert_mem_equal(&got, &up, sizeof(got));
	zassert_equal(fw_store_get("upload_0000000000000000", 6, &got), -ENOENT);
	zassert_equal(fw_store_get(NULL, 6, &got), -ENOENT);
}

ZTEST(firmware_store, test_ids_differ_between_uploads)
{
	const struct fixture *fx = fixture("valid");
	char first[FW_STORE_ID_LEN + 1];

	create_for(fx, 0);
	strcpy(first, up.id);
	zassert_ok(fw_store_delete(up.id));
	create_for(fx, 0);
	zassert_true(strcmp(first, up.id) != 0);
}

ZTEST(firmware_store, test_create_refusals)
{
	static char long_name[FW_STORE_FILENAME_MAX + 2];

	memset(digest, 0xAB, sizeof(digest));
	memset(long_name, 'n', FW_STORE_FILENAME_MAX + 1);
	zassert_equal(fw_store_create("", 100, digest, 0, NULL), -EINVAL);
	zassert_equal(fw_store_create(NULL, 100, digest, 0, NULL), -EINVAL);
	zassert_equal(fw_store_create("x.bin", 0, digest, 0, NULL), -EINVAL);
	zassert_equal(fw_store_create("x.bin", 100, NULL, 0, NULL), -EINVAL);
	zassert_equal(fw_store_create(long_name, 100, digest, 0, NULL), -EINVAL);
	long_name[FW_STORE_FILENAME_MAX] = '\0';

	zassert_equal(fw_store_create("x.bin", CONFIG_FIRMWARE_STORE_MAX_BYTES + 1U, digest, 0,
				      NULL),
		      -EFBIG);
	/* The 1 MiB test volume cannot hold the largest image. */
	zassert_equal(fw_store_create("x.bin", CONFIG_FIRMWARE_STORE_MAX_BYTES, digest, 0, NULL),
		      -ENOSPC);
	zassert_false(exists(META));

	zassert_ok(fw_store_create(long_name, 100, digest, 0, &up));
	zassert_equal(strlen(up.filename), FW_STORE_FILENAME_MAX);
	/* One upload: a second waits for the first to be deleted. */
	zassert_equal(fw_store_create("y.bin", 100, digest, 0, NULL), -EBUSY);
	/* Busy outranks too large, as the mock answers. */
	zassert_equal(fw_store_create("y.bin", CONFIG_FIRMWARE_STORE_MAX_BYTES + 1U, digest, 0,
				      NULL),
		      -EBUSY);
}

ZTEST(firmware_store, test_filename_is_never_a_path)
{
	struct fs_dir_t dir;
	struct fs_dirent e;
	size_t names = 0;

	memset(digest, 1, sizeof(digest));
	zassert_ok(fw_store_create("../../lfs/settings", 64, digest, 0, &up));
	zassert_ok(fw_store_chunk_accept(up.id, 0, digest, 32, 0));
	zassert_ok(fw_store_chunk_commit(up.id, 0));
	zassert_str_equal(up.filename, "../../lfs/settings");

	fs_dir_t_init(&dir);
	zassert_ok(fs_opendir(&dir, TEST_DIR));
	while (fs_readdir(&dir, &e) == 0 && e.name[0] != '\0') {
		zassert_true(strcmp(e.name, "upload.bin") == 0 || strcmp(e.name, "upload.meta") == 0,
			     "unexpected %s", e.name);
		names++;
	}
	fs_closedir(&dir);
	zassert_equal(names, 2U);
	zassert_false(exists(TEST_MNT "/lfs"));
}

ZTEST(firmware_store, test_create_before_init_succeeded)
{
	memset(digest, 1, sizeof(digest));
	/* A directory on no mounted volume. */
	zassert_equal(fw_store_init("/nowhere/firmware", 0), -EIO);
	zassert_equal(fw_store_create("x.bin", 100, digest, 0, NULL), -EAGAIN);
	zassert_equal(fw_store_get("upload_0000000000000000", 0, &up), -ENOENT);
	zassert_equal(fw_store_init(TEST_DIR, 0), 0);
}

ZTEST(firmware_store, test_init_rejects_bad_directories)
{
	static char long_dir[128];

	memset(long_dir, 'd', sizeof(long_dir) - 1);
	zassert_equal(fw_store_init(NULL, 0), -EINVAL);
	zassert_equal(fw_store_init("", 0), -EINVAL);
	zassert_equal(fw_store_init(long_dir, 0), -EINVAL);
}

/* -- chunks ------------------------------------------------------------------------- */

ZTEST(firmware_store, test_chunk_refusals_in_order)
{
	memset(digest, 2, sizeof(digest));
	zassert_ok(fw_store_create("x.bin", 100, digest, 0, &up));

	zassert_equal(fw_store_chunk_accept("upload_ffffffffffffffff", 0, big, 10, 0), -ENOENT);
	zassert_equal(fw_store_chunk_accept(NULL, 0, big, 10, 0), -ENOENT);
	/* A wrong offset outranks an empty chunk. */
	zassert_equal(fw_store_chunk_accept(up.id, 5, big, 0, 0), -ERANGE);
	zassert_equal(fw_store_chunk_accept(up.id, 0, big, 0, 0), -ENODATA);
	zassert_equal(fw_store_chunk_accept(up.id, 0, NULL, 10, 0), -ENODATA);
	/* Too large a chunk outranks running past the declared size. */
	zassert_equal(fw_store_chunk_accept(up.id, 0, big, CHUNK + 1U, 0), -E2BIG);
	zassert_equal(fw_store_chunk_accept(up.id, 0, big, 101, 0), -EOVERFLOW);

	zassert_ok(fw_store_chunk_accept(up.id, 0, big, 60, 0));
	/* Pending outranks a wrong offset. */
	zassert_equal(fw_store_chunk_accept(up.id, 7, big, 10, 0), -EBUSY);
	zassert_equal(fw_store_chunk_accept(up.id, 0, big, 60, 0), -EBUSY);
	zassert_ok(fw_store_chunk_commit(up.id, 0));
	zassert_equal(fw_store_chunk_accept(up.id, 60, big, 41, 0), -EOVERFLOW);
	zassert_ok(fw_store_chunk_accept(up.id, 60, big, 40, 0));
	zassert_ok(fw_store_chunk_commit(up.id, 0));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 100U);
	/* Everything received: nothing more fits. */
	zassert_equal(fw_store_chunk_accept(up.id, 100, big, 1, 0), -EOVERFLOW);
}

ZTEST(firmware_store, test_a_chunk_counts_only_once_committed)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	zassert_ok(fw_store_chunk_accept(up.id, 0, fx->data, CHUNK, 0));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 0U);
	zassert_true(up.chunk_pending);
	zassert_false(exists(BIN));

	zassert_ok(fw_store_chunk_commit(up.id, 0));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, (uint32_t)CHUNK);
	zassert_false(up.chunk_pending);
	zassert_equal(size_of(BIN), (size_t)CHUNK);

	zassert_equal(fw_store_chunk_commit(up.id, 0), -ENODATA);
	zassert_equal(fw_store_chunk_commit("upload_ffffffffffffffff", 0), -ENOENT);

	/* The metadata says so too. */
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, (uint32_t)CHUNK);
}

ZTEST(firmware_store, test_staged_bytes_are_a_copy)
{
	const struct fixture *fx = fixture("valid");
	static uint8_t chunk[CHUNK];

	create_for(fx, 0);
	memcpy(chunk, fx->data, CHUNK);
	zassert_ok(fw_store_chunk_accept(up.id, 0, chunk, CHUNK, 0));
	/* The HTTP body buffer is reused after the callback returns. */
	memset(chunk, 0x5A, sizeof(chunk));
	zassert_ok(fw_store_chunk_commit(up.id, 0));
	send(fx, CHUNK, fx->len, 0);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_READY, "%s", up.error_message);
}

ZTEST(firmware_store, test_discarded_chunk_can_be_sent_again)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	zassert_ok(fw_store_chunk_accept(up.id, 0, fx->data, 100, 0));
	fw_store_chunk_discard("upload_ffffffffffffffff");
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_true(up.chunk_pending);
	fw_store_chunk_discard(up.id);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_false(up.chunk_pending);
	zassert_equal(fw_store_chunk_commit(up.id, 0), -ENODATA);
	zassert_ok(fw_store_chunk_accept(up.id, 0, fx->data, 100, 0));
}

/* -- verify ------------------------------------------------------------------------- */

ZTEST(firmware_store, test_valid_upload_becomes_ready)
{
	const struct fixture *fx = fixture("valid");

	upload(fx, 0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, fx->len);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_READY, "%s", up.error_message);
	zassert_true(up.has_image);
	zassert_str_equal(up.version, "1.2.3-synthetic");
	zassert_str_equal(up.project_name, "eh_cp_c6_cedar");
	zassert_str_equal(up.idf_version, "v5.5.5-synthetic");
	zassert_str_equal(up.layout_id, "cedar-c6-ota-4m-2x1792k");
	zassert_str_equal(up.host_protocol, "esp-hosted-mcu-3");
	zassert_equal(up.bootloader_bytes, 336U);
	zassert_equal(up.app_bytes, 1040U);
	zassert_str_equal(up.error_code, "");
	zassert_equal(fw_store_chunk_accept(up.id, up.received_bytes, big, 1, 0), -EINVAL);
}

ZTEST(firmware_store, test_corpus_outcomes_through_the_store)
{
	for (size_t i = 0; i < corpus_count; i++) {
		const struct fixture *fx = &corpus[i];

		upload(fx, 0);
		verify(0);
		if (fx->code == NULL) {
			zassert_equal(up.state, FW_UPLOAD_READY, "%s: %s", fx->name, up.error_message);
		} else {
			zassert_equal(up.state, FW_UPLOAD_FAILED, "%s", fx->name);
			zassert_false(up.has_image, "%s", fx->name);
			zassert_str_equal(up.error_code, fx->code, "%s", fx->name);
			zassert_not_null(strstr(up.error_message, fx->message), "%s: %s", fx->name,
					 up.error_message);
			zassert_str_equal(up.version, "", "%s", fx->name);
		}
		zassert_ok(fw_store_delete(up.id), "%s", fx->name);
	}
}

ZTEST(firmware_store, test_declared_digest_outranks_the_structure)
{
	const struct fixture *valid = fixture("valid");
	const struct fixture *junk = fixture("junk_in_nvs");

	/* The right file with a wrong declared digest. */
	sha256_of(valid->data, valid->len, digest);
	digest[31] ^= 0x01;
	zassert_ok(fw_store_create("x.bin", (uint32_t)valid->len, digest, 0, &up));
	send(valid, 0, valid->len, 0);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_FAILED);
	zassert_str_equal(up.error_code, "invalid_image");
	zassert_not_null(strstr(up.error_message, "does not match the one given"));
	zassert_ok(fw_store_delete(up.id));

	/* A damaged file declared as the valid one: the transfer is the likelier fault. */
	sha256_of(valid->data, valid->len, digest);
	zassert_ok(fw_store_create("x.bin", (uint32_t)junk->len, digest, 0, &up));
	send(junk, 0, junk->len, 0);
	verify(0);
	zassert_not_null(strstr(up.error_message, "does not match the one given"),
			 "%s", up.error_message);
}

ZTEST(firmware_store, test_verify_begin_refusals)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	zassert_equal(fw_store_verify_begin("upload_ffffffffffffffff", 0), -ENOENT);
	zassert_equal(fw_store_verify_begin(up.id, 0), -EINVAL);
	zassert_equal(fw_store_verify(up.id, NULL, NULL, NULL, 0), -ENOENT);

	send(fx, 0, CHUNK, 0);
	zassert_ok(fw_store_chunk_accept(up.id, CHUNK, &fx->data[CHUNK], CHUNK, 0));
	zassert_equal(fw_store_verify_begin(up.id, 0), -EBUSY);
	zassert_ok(fw_store_chunk_commit(up.id, 0));
	send(fx, 2 * CHUNK, fx->len, 0);

	zassert_ok(fw_store_verify_begin(up.id, 0));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_VERIFYING);
	zassert_equal(fw_store_verify_begin(up.id, 0), -EINVAL);
	/* Nothing more is taken while the check runs. */
	zassert_equal(fw_store_chunk_accept(up.id, up.received_bytes, big, 1, 0), -EINVAL);
	zassert_equal(fw_store_delete(up.id), -EBUSY);
	zassert_ok(fw_store_verify(up.id, NULL, NULL, NULL, 0));
	zassert_equal(fw_store_verify_begin(up.id, 0), -EINVAL);
}

ZTEST(firmware_store, test_failed_upload_is_checked_again_or_replaced)
{
	const struct fixture *fx = fixture("junk_in_nvs");
	char old[FW_STORE_ID_LEN + 1];

	upload(fx, 0);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_FAILED);
	zassert_ok(fw_store_verify_begin(up.id, 0));
	zassert_ok(fw_store_verify(up.id, NULL, NULL, NULL, 0));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_FAILED);

	strcpy(old, up.id);
	create_for(fixture("valid"), 0);
	zassert_true(strcmp(old, up.id) != 0);
	zassert_equal(fw_store_get(old, 0, &up), -ENOENT);
	zassert_false(exists(BIN));
}

struct cancel_probe {
	int calls;
	int cancel_after;
	uint32_t last_done;
	uint32_t last_total;
};

static bool cancel_cb(void *ctx)
{
	struct cancel_probe *p = ctx;

	return p->cancel_after >= 0 && p->calls >= p->cancel_after;
}

static void progress_cb(void *ctx, uint32_t done, uint32_t total)
{
	struct cancel_probe *p = ctx;

	zassert_true(done > p->last_done);
	p->calls++;
	p->last_done = done;
	p->last_total = total;
}

ZTEST(firmware_store, test_progress_counts_every_byte)
{
	const struct fixture *fx = fixture("valid");
	struct cancel_probe probe = {.cancel_after = -1};

	upload(fx, 0);
	zassert_ok(fw_store_verify_begin(up.id, 0));
	zassert_ok(fw_store_verify(up.id, cancel_cb, progress_cb, &probe, 0));
	zassert_equal(probe.last_done, fx->len);
	zassert_equal(probe.last_total, fx->len);
	zassert_equal(probe.calls,
		      (int)DIV_ROUND_UP(fx->len, (size_t)CONFIG_FIRMWARE_STORE_READ_BUF));
}

ZTEST(firmware_store, test_cancelled_check_receives_again)
{
	const struct fixture *fx = fixture("valid");
	struct cancel_probe probe = {.cancel_after = 2};

	upload(fx, 0);
	zassert_ok(fw_store_verify_begin(up.id, 0));
	zassert_equal(fw_store_verify(up.id, cancel_cb, progress_cb, &probe, 0), -ECANCELED);
	zassert_equal(probe.calls, 2);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_RECEIVING);
	zassert_equal(up.received_bytes, fx->len);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_READY);
}

ZTEST(firmware_store, test_cancelled_recheck_of_failed_upload_receives_again)
{
	const struct fixture *fx = fixture("junk_in_nvs");
	struct cancel_probe probe = {.cancel_after = 0};

	upload(fx, 0);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_FAILED);
	zassert_ok(fw_store_verify_begin(up.id, 0));
	zassert_equal(fw_store_verify(up.id, cancel_cb, progress_cb, &probe, 0), -ECANCELED);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_RECEIVING);
	zassert_str_equal(up.error_code, "");
	/* And it stays so after a reboot. */
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_RECEIVING);
}

ZTEST(firmware_store, test_unreadable_file_leaves_the_state)
{
	const struct fixture *fx = fixture("valid");

	upload(fx, 0);
	zassert_ok(fw_store_verify_begin(up.id, 0));
	zassert_ok(fs_unlink(BIN));
	zassert_equal(fw_store_verify(up.id, NULL, NULL, NULL, 0), -EIO);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_RECEIVING);
}

ZTEST(firmware_store, test_file_shorter_than_its_size_fails_the_check)
{
	const struct fixture *fx = fixture("valid");

	upload(fx, 0);
	zassert_ok(fw_store_verify_begin(up.id, 0));
	truncate_to(BIN, fx->len - 1U);
	zassert_ok(fw_store_verify(up.id, NULL, NULL, NULL, 0));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_FAILED);
	zassert_not_null(strstr(up.error_message, "shorter than its declared size"));
}

/* -- recovery ----------------------------------------------------------------------- */

ZTEST(firmware_store, test_reboot_keeps_offset_and_outcome)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	send(fx, 0, 2 * CHUNK, 0);
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_RECEIVING);
	zassert_equal(up.received_bytes, 2U * CHUNK);
	zassert_str_equal(up.filename, "cedar-c6.bin");

	send(fx, 2 * CHUNK, fx->len, 0);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_READY);

	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_READY);
	zassert_str_equal(up.layout_id, "cedar-c6-ota-4m-2x1792k");
	zassert_equal(up.app_bytes, 1040U);
	zassert_false(up.in_use);
	zassert_str_equal(up.active_job_id, "");
}

ZTEST(firmware_store, test_verifying_is_receiving_after_a_reboot)
{
	const struct fixture *fx = fixture("valid");

	upload(fx, 0);
	zassert_ok(fw_store_verify_begin(up.id, 0));
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_RECEIVING);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_READY);
}

ZTEST(firmware_store, test_chunk_on_file_but_not_in_metadata_is_cut)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	send(fx, 0, CHUNK, 0);
	/* The worker wrote the next chunk's data, then the power went. */
	write_file(BIN, &fx->data[CHUNK], 100, true);
	zassert_equal(size_of(BIN), (size_t)CHUNK + 100U);

	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, (uint32_t)CHUNK);
	zassert_equal(size_of(BIN), (size_t)CHUNK);
	send(fx, CHUNK, fx->len, 0);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_READY);
}

ZTEST(firmware_store, test_file_shorter_than_metadata_moves_the_offset_back)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	send(fx, 0, 2 * CHUNK, 0);
	truncate_to(BIN, 1000);
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 1000U);
	/* Persisted, not only in RAM. */
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 1000U);
	send(fx, 1000, fx->len, 0);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_READY);
}

ZTEST(firmware_store, test_commit_refuses_when_committed_bytes_vanished)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	send(fx, 0, CHUNK, 0);
	/* Something outside the store shortened the file. */
	truncate_to(BIN, 100);
	zassert_ok(fw_store_chunk_accept(up.id, CHUNK, &fx->data[CHUNK], CHUNK, 0));
	zassert_equal(fw_store_chunk_commit(up.id, 0), -EIO);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, (uint32_t)CHUNK);
	zassert_false(up.chunk_pending);
	/* A reboot reconciles it. */
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 100U);
}

ZTEST(firmware_store, test_metadata_without_data_file_starts_over)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	send(fx, 0, CHUNK, 0);
	zassert_ok(fs_unlink(BIN));
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, 0U);
	send(fx, 0, fx->len, 0);
}

ZTEST(firmware_store, test_damaged_ready_file_receives_again)
{
	const struct fixture *fx = fixture("valid");

	upload(fx, 0);
	verify(0);
	truncate_to(BIN, fx->len - 1U);
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_RECEIVING);
	zassert_equal(up.received_bytes, fx->len - 1U);
	zassert_false(up.has_image);
	zassert_str_equal(up.error_code, "");
}

ZTEST(firmware_store, test_damaged_failed_file_receives_again)
{
	const struct fixture *fx = fixture("junk_in_nvs");

	upload(fx, 0);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_FAILED);
	truncate_to(BIN, 10);
	reboot(0);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.state, FW_UPLOAD_RECEIVING);
	zassert_equal(up.received_bytes, 10U);
	zassert_str_equal(up.error_code, "");
}

static void corrupt_meta_at(size_t offset)
{
	struct fs_file_t f;
	uint8_t b;

	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, META, FS_O_RDWR));
	zassert_ok(fs_seek(&f, offset, FS_SEEK_SET));
	zassert_equal(fs_read(&f, &b, 1), 1);
	b ^= 0x40;
	zassert_ok(fs_seek(&f, offset, FS_SEEK_SET));
	zassert_equal(fs_write(&f, &b, 1), 1);
	zassert_ok(fs_close(&f));
}

ZTEST(firmware_store, test_corrupt_metadata_removes_the_upload)
{
	const struct fixture *fx = fixture("valid");
	const size_t meta_len = 0;

	ARG_UNUSED(meta_len);
	/* A byte anywhere: magic, state, id, sizes, the CRC itself. */
	static const size_t offsets[] = {0, 5, 6, 10, 600, 700};

	for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
		create_for(fx, 0);
		send(fx, 0, CHUNK, 0);
		corrupt_meta_at(MIN(offsets[i], size_of(META) - 1U));
		reboot(0);
		zassert_equal(fw_store_get(up.id, 0, &up), -ENOENT, "offset %zu", offsets[i]);
		zassert_false(exists(BIN));
		zassert_false(exists(META));
	}

	/* The last byte (the CRC) and a truncated file. */
	create_for(fx, 0);
	corrupt_meta_at(size_of(META) - 1U);
	reboot(0);
	zassert_equal(fw_store_get(up.id, 0, &up), -ENOENT);

	create_for(fx, 0);
	truncate_to(META, 10);
	reboot(0);
	zassert_equal(fw_store_get(up.id, 0, &up), -ENOENT);

	/* One byte too long. */
	create_for(fx, 0);
	write_file(META, digest, 1, true);
	reboot(0);
	zassert_equal(fw_store_get(up.id, 0, &up), -ENOENT);
}

/* Metadata a foreign writer produced with a correct CRC is still judged by its fields. */
ZTEST(firmware_store, test_consistent_metadata_with_a_short_id_is_removed)
{
	static uint8_t meta[1024];
	struct fs_file_t f;
	const size_t id_at = 8U; /* magic 4, version 2, state 1, has_image 1 */
	ssize_t n;

	create_for(fixture("valid"), 0);
	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, META, FS_O_READ));
	n = fs_read(&f, meta, sizeof(meta));
	zassert_ok(fs_close(&f));
	zassert_true(n > 32 && n < (ssize_t)sizeof(meta));

	/* "upload_" and 8 hex digits instead of 16, then the CRC over everything before it. */
	meta[id_at + 15U] = '\0';
	sys_put_le32(crc32_ieee(meta, (size_t)n - 4U), &meta[n - 4]);
	write_file(META, meta, (size_t)n, false);

	reboot(0);
	zassert_equal(fw_store_get(up.id, 0, &up), -ENOENT);
	zassert_false(exists(META));
}

ZTEST(firmware_store, test_data_file_without_metadata_is_removed)
{
	const struct fixture *fx = fixture("valid");

	write_file(BIN, fx->data, 100, false);
	reboot(0);
	zassert_false(exists(BIN));
	/* And create does not count a stray one either. */
	write_file(BIN, fx->data, 100, false);
	create_for(fx, 0);
	zassert_false(exists(BIN));
}

ZTEST(firmware_store, test_leftovers_go_other_files_stay)
{
	static const char *const leftovers[] = {
		"upload.meta.tmp", "upload.old", "uploadX", "upload.1", "upload.2",
		"upload.3",        "upload.4",   "upload.5",
	};
	char path[64];

	create_for(fixture("valid"), 0);
	for (size_t i = 0; i < ARRAY_SIZE(leftovers); i++) {
		snprintf(path, sizeof(path), TEST_DIR "/%s", leftovers[i]);
		write_file(path, digest, 4, false);
	}
	write_file(TEST_DIR "/update.journal", digest, 4, false);
	write_file(TEST_DIR "/notes.txt", digest, 4, false);

	reboot(0);
	for (size_t i = 0; i < ARRAY_SIZE(leftovers); i++) {
		snprintf(path, sizeof(path), TEST_DIR "/%s", leftovers[i]);
		zassert_false(exists(path), "%s", leftovers[i]);
	}
	zassert_true(exists(TEST_DIR "/update.journal"));
	zassert_true(exists(TEST_DIR "/notes.txt"));
	zassert_true(exists(META));
	zassert_ok(fw_store_get(up.id, 0, &up));
}

ZTEST(firmware_store, test_init_creates_the_directory)
{
	zassert_ok(fw_store_init(TEST_MNT "/elsewhere", 0));
	zassert_true(exists(TEST_MNT "/elsewhere"));
	memset(digest, 3, sizeof(digest));
	zassert_ok(fw_store_create("x.bin", 10, digest, 0, &up));
	zassert_true(exists(TEST_MNT "/elsewhere/upload.meta"));
}

/* -- space -------------------------------------------------------------------------- */

static void fill_until_free_below(uint64_t bytes)
{
	static uint8_t junk[4096];
	struct fs_statvfs sv;
	struct fs_file_t f;

	memset(junk, 0x33, sizeof(junk));
	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, TEST_MNT "/junk", FS_O_CREATE | FS_O_WRITE | FS_O_APPEND));
	for (;;) {
		zassert_ok(fs_statvfs(TEST_MNT, &sv));
		if ((uint64_t)sv.f_bfree * sv.f_frsize < bytes) {
			break;
		}
		ssize_t n = fs_write(&f, junk, sizeof(junk));

		if (n == -ENOSPC || (n >= 0 && n < (ssize_t)sizeof(junk)) ||
		    fs_sync(&f) == -ENOSPC) {
			break; /* The volume is full: as far as it goes. */
		}
		zassert_equal(n, (ssize_t)sizeof(junk));
	}
	(void)fs_close(&f);
}

ZTEST(firmware_store, test_storage_full_at_create)
{
	struct fs_statvfs sv;

	memset(digest, 4, sizeof(digest));
	zassert_ok(fs_statvfs(TEST_MNT, &sv));
	const uint64_t free = (uint64_t)sv.f_bfree * sv.f_frsize;
	const uint64_t reserve = (uint64_t)CONFIG_FIRMWARE_STORE_RESERVE_KIB * 1024U;

	/* Rounded up to the allocation unit, plus the reserve. */
	const uint32_t fits = (uint32_t)(free - reserve) / sv.f_frsize * sv.f_frsize;

	zassert_equal(fw_store_create("x.bin", fits + 1U, digest, 0, NULL), -ENOSPC);
	zassert_ok(fw_store_create("x.bin", fits, digest, 0, &up));
}

ZTEST(firmware_store, test_storage_full_at_commit)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	send(fx, 0, CHUNK, 0);
	/*
	 * Another writer takes the volume after create's check. The store no longer
	 * asks for free space per chunk (fs_statvfs() walks all of LittleFS), so the
	 * refusal is LittleFS's own -ENOSPC on the write: less than a chunk is left.
	 */
	fill_until_free_below(CHUNK);

	zassert_ok(fw_store_chunk_accept(up.id, CHUNK, &fx->data[CHUNK], CHUNK, 0));
	zassert_equal(fw_store_chunk_commit(up.id, 0), -ENOSPC);
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(up.received_bytes, (uint32_t)CHUNK);
	zassert_false(up.chunk_pending);
	zassert_equal(size_of(BIN), (size_t)CHUNK);

	/* Room again: the same offset goes through. */
	zassert_ok(fs_unlink(TEST_MNT "/junk"));
	send(fx, CHUNK, fx->len, 0);
	verify(0);
	zassert_equal(up.state, FW_UPLOAD_READY);
}

/* -- expiry ------------------------------------------------------------------------- */

ZTEST(firmware_store, test_expiry_after_a_day_of_uptime)
{
	create_for(fixture("valid"), 1000);
	zassert_ok(fw_store_get(up.id, 1000 + EXPIRE_MS - 1, &up));
	zassert_equal(fw_store_get(up.id, 1000 + EXPIRE_MS, &up), -ENOENT);
	zassert_false(exists(META));
}

ZTEST(firmware_store, test_reading_is_not_activity)
{
	create_for(fixture("valid"), 0);
	zassert_ok(fw_store_get(up.id, EXPIRE_MS / 2, &up));
	zassert_ok(fw_store_get(up.id, EXPIRE_MS - 1, &up));
	fw_store_tick(EXPIRE_MS);
	zassert_false(exists(META));
}

ZTEST(firmware_store, test_chunks_and_checks_are_activity)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	zassert_ok(fw_store_chunk_accept(up.id, 0, fx->data, CHUNK, EXPIRE_MS - 10));
	zassert_ok(fw_store_chunk_commit(up.id, EXPIRE_MS - 1));
	zassert_ok(fw_store_get(up.id, 2 * EXPIRE_MS - 2, &up));
	zassert_equal(fw_store_get(up.id, 2 * EXPIRE_MS - 1, &up), -ENOENT);

	upload(fx, 0);
	zassert_ok(fw_store_verify_begin(up.id, EXPIRE_MS - 1));
	zassert_ok(fw_store_verify(up.id, NULL, NULL, NULL, EXPIRE_MS));
	zassert_ok(fw_store_get(up.id, 2 * EXPIRE_MS - 1, &up));
	fw_store_tick(2 * EXPIRE_MS);
	zassert_equal(fw_store_get(up.id, 2 * EXPIRE_MS, &up), -ENOENT);
}

ZTEST(firmware_store, test_expired_upload_takes_no_chunk)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	zassert_equal(fw_store_chunk_accept(up.id, 0, fx->data, 10, EXPIRE_MS), -ENOENT);
	zassert_false(exists(META));
}

ZTEST(firmware_store, test_a_staged_chunk_is_activity)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	zassert_ok(fw_store_chunk_accept(up.id, 0, fx->data, 10, EXPIRE_MS - 5));
	fw_store_chunk_discard(up.id);
	zassert_ok(fw_store_get(up.id, 2 * EXPIRE_MS - 6, &up));
	zassert_equal(fw_store_get(up.id, 2 * EXPIRE_MS - 5, &up), -ENOENT);
}

ZTEST(firmware_store, test_expiry_waits_for_install_pending_chunk_and_reader)
{
	const struct fixture *fx = fixture("valid");

	upload(fx, 0);
	verify(0);
	zassert_ok(fw_store_set_in_use(up.id, true));
	fw_store_tick(3 * EXPIRE_MS);
	zassert_ok(fw_store_get(up.id, 3 * EXPIRE_MS, &up));
	zassert_ok(fw_store_set_in_use(up.id, false));
	fw_store_tick(3 * EXPIRE_MS);
	zassert_equal(fw_store_get(up.id, 3 * EXPIRE_MS, &up), -ENOENT);

	upload(fx, 0);
	verify(0);
	zassert_ok(fw_store_image_open(up.id, NULL));
	fw_store_tick(3 * EXPIRE_MS);
	zassert_ok(fw_store_get(up.id, 3 * EXPIRE_MS, &up));
	fw_store_image_close();
	zassert_ok(fw_store_delete(up.id));

	create_for(fx, 0);
	zassert_ok(fw_store_chunk_accept(up.id, 0, fx->data, 10, 0));
	zassert_ok(fw_store_get(up.id, 3 * EXPIRE_MS, &up));
	zassert_ok(fw_store_chunk_commit(up.id, 0));
	zassert_equal(fw_store_get(up.id, 3 * EXPIRE_MS, &up), -ENOENT);

	create_for(fx, 0);
	send(fx, 0, fx->len, 0);
	zassert_ok(fw_store_verify_begin(up.id, 0));
	zassert_ok(fw_store_get(up.id, 3 * EXPIRE_MS, &up));
	zassert_ok(fw_store_verify(up.id, NULL, NULL, NULL, 0));
}

ZTEST(firmware_store, test_expiry_period_restarts_with_the_boot)
{
	create_for(fixture("valid"), 5 * EXPIRE_MS);
	reboot(10);
	zassert_ok(fw_store_get(up.id, 10 + EXPIRE_MS - 1, &up));
	zassert_equal(fw_store_get(up.id, 10 + EXPIRE_MS, &up), -ENOENT);
}

ZTEST(firmware_store, test_expired_upload_frees_create)
{
	create_for(fixture("valid"), 0);
	create_for(fixture("valid"), EXPIRE_MS);
	zassert_ok(fw_store_get(up.id, EXPIRE_MS, &up));
}

/* -- delete, marks, reader ---------------------------------------------------------- */

ZTEST(firmware_store, test_delete)
{
	const struct fixture *fx = fixture("valid");

	zassert_equal(fw_store_delete("upload_ffffffffffffffff"), -ENOENT);
	upload(fx, 0);
	verify(0);

	zassert_ok(fw_store_set_in_use(up.id, true));
	zassert_equal(fw_store_delete(up.id), -EBUSY);
	zassert_equal(fw_store_create("x.bin", 10, digest, 0, NULL), -EBUSY);
	zassert_ok(fw_store_set_in_use(up.id, false));

	zassert_ok(fw_store_image_open(up.id, NULL));
	zassert_equal(fw_store_delete(up.id), -EBUSY);
	fw_store_image_close();

	zassert_ok(fw_store_delete(up.id));
	zassert_false(exists(BIN));
	zassert_false(exists(META));
	zassert_equal(fw_store_get(up.id, 0, &up), -ENOENT);

	create_for(fx, 0);
	zassert_ok(fw_store_chunk_accept(up.id, 0, fx->data, 10, 0));
	zassert_equal(fw_store_delete(up.id), -EBUSY);
	zassert_ok(fw_store_chunk_commit(up.id, 0));
	zassert_ok(fw_store_delete(up.id));
}

ZTEST(firmware_store, test_ready_upload_is_not_replaced)
{
	upload(fixture("valid"), 0);
	verify(0);
	zassert_equal(fw_store_create("x.bin", 10, digest, 0, NULL), -EBUSY);
}

ZTEST(firmware_store, test_install_and_job_marks)
{
	const struct fixture *fx = fixture("valid");

	create_for(fx, 0);
	zassert_equal(fw_store_set_in_use(up.id, true), -EINVAL);
	zassert_ok(fw_store_set_in_use(up.id, false));
	zassert_equal(fw_store_set_in_use("upload_ffffffffffffffff", false), -ENOENT);

	zassert_ok(fw_store_set_active_job(up.id, "job_0001"));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_str_equal(up.active_job_id, "job_0001");
	zassert_ok(fw_store_set_active_job(up.id, NULL));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_str_equal(up.active_job_id, "");
	zassert_ok(fw_store_set_active_job(up.id, "job_that_is_far_longer_than_the_thirty_one"));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_equal(strlen(up.active_job_id), FW_STORE_JOB_ID_MAX);
	zassert_equal(fw_store_set_active_job("upload_ffffffffffffffff", "x"), -ENOENT);

	send(fx, 0, fx->len, 0);
	verify(0);
	zassert_ok(fw_store_set_in_use(up.id, true));
	zassert_ok(fw_store_get(up.id, 0, &up));
	zassert_true(up.in_use);
	zassert_equal(fw_store_verify_begin(up.id, 0), -EBUSY);
}

ZTEST(firmware_store, test_image_reader)
{
	const struct fixture *fx = fixture("valid");
	static uint8_t buf[5000];
	uint32_t size = 0;

	create_for(fx, 0);
	zassert_equal(fw_store_image_open("upload_ffffffffffffffff", &size), -ENOENT);
	zassert_equal(fw_store_image_open(up.id, &size), -EINVAL);
	zassert_equal(fw_store_image_read(0, buf, 10), -EBADF);

	send(fx, 0, fx->len, 0);
	zassert_ok(fw_store_verify_begin(up.id, 0));
	zassert_equal(fw_store_image_open(up.id, &size), -EINVAL);
	zassert_ok(fw_store_verify(up.id, NULL, NULL, NULL, 0));

	zassert_ok(fw_store_image_open(up.id, &size));
	zassert_equal(size, fx->len);
	zassert_equal(fw_store_image_open(up.id, &size), -EBUSY);

	/* Out of order, as a retrying writer reads. */
	zassert_equal(fw_store_image_read(0x10000, buf, sizeof(buf)), (int)(fx->len - 0x10000));
	zassert_mem_equal(buf, &fx->data[0x10000], fx->len - 0x10000);
	zassert_equal(fw_store_image_read(0, buf, sizeof(buf)), (int)sizeof(buf));
	zassert_mem_equal(buf, fx->data, sizeof(buf));
	zassert_equal(fw_store_image_read(0x8000, buf, 32), 32);
	zassert_mem_equal(buf, &fx->data[0x8000], 32);
	zassert_equal(fw_store_image_read((uint32_t)fx->len, buf, 10), 0);

	fw_store_image_close();
	fw_store_image_close();
	zassert_equal(fw_store_image_read(0, buf, 10), -EBADF);
	zassert_ok(fw_store_image_open(up.id, &size));
	fw_store_image_close();
}

ZTEST(firmware_store, test_state_names)
{
	zassert_str_equal(fw_upload_state_str(FW_UPLOAD_RECEIVING), "receiving");
	zassert_str_equal(fw_upload_state_str(FW_UPLOAD_VERIFYING), "verifying");
	zassert_str_equal(fw_upload_state_str(FW_UPLOAD_READY), "ready");
	zassert_str_equal(fw_upload_state_str(FW_UPLOAD_FAILED), "failed");
	zassert_str_equal(fw_upload_state_str((enum fw_upload_state)9), "unknown");
}
