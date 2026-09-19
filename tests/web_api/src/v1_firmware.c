/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The firmware upload bindings (src/web/api/v1/firmware.c) and cancelJob through
 * the real route table, over the real firmware-store on LittleFS in a 1 MiB
 * sim-flash partition, with the v1 job worker doing the file I/O. Part of the v1
 * suite, whose before() closes the bindings; each test here opens a fresh store.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <coprocessor_updater/coprocessor_updater.h>
#include <firmware_store/firmware_store.h>
#include <job_manager/job_manager.h>
#include <log_store/log_store.h>

#include "fake_update_platform.h"

#include "harness.h"
#include "v1_internal.h"
#include "web_api_v1.h"

#define PASSWORD   "correct horse battery"
#define LAN_ORIGIN "http://192.168.88.14"
#define MNT        "/fw"
#define DIR        "/fw/firmware"
#define CHUNK      16384U

static const uint8_t fx_valid[] = {
#include "fw_valid.inc"
};
static const uint8_t fx_bare_app[] = {
#include "fw_bare_app.inc"
};

static char fw_cookie[96];
static char fw_csrf[WEB_AUTH_TOKEN_LEN + 1];
static int64_t fw_now;

static int64_t fw_clock(void)
{
	return fw_now;
}

static const struct web_api_v1_firmware fw_hooks = {.now_ms = fw_clock};

/* -- the volume ------------------------------------------------------------------ */

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(fw_lfs);

static struct fs_mount_t fw_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &fw_lfs,
	.storage_dev = (void *)PARTITION_ID(fw_test_partition),
	.mnt_point = MNT,
};
static bool fw_mounted;

/* -- a worker that can be held, so work stays queued ------------------------------ */

static K_SEM_DEFINE(gate, 0, 1);

static void gate_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	k_sem_take(&gate, K_FOREVER);
}

static K_WORK_DEFINE(gate_work, gate_fn);

static void hold_worker(void)
{
	k_sem_reset(&gate);
	zassert_true(v1_worker_submit(&gate_work) >= 0);
	k_msleep(20);
}

static void release_worker(void)
{
	k_sem_give(&gate);
}

/* -- helpers ------------------------------------------------------------------------ */

static bool json_string(const char *name, char *out, size_t cap)
{
	char key[48];
	const char *p;
	const char *end;

	snprintf(key, sizeof(key), "\"%s\":\"", name);
	p = strstr(body, key);
	if (p == NULL) {
		return false;
	}
	p += strlen(key);
	end = strchr(p, '"');
	if (end == NULL || (size_t)(end - p) >= cap) {
		return false;
	}
	memcpy(out, p, end - p);
	out[end - p] = '\0';
	return true;
}

static void sign_in(void)
{
	char token[WEB_AUTH_SETUP_TOKEN_LEN + 1];
	const char *set_cookie;
	const char *semi;

	request(WEB_API_GET, "/api/v1/auth/state");
	dispatch(&web_api_v1_router);
	zassert_true(json_string("setup_token", token, sizeof(token)));
	request(WEB_API_POST, "/api/v1/auth/setup");
	ctx.req.headers.origin = LAN_ORIGIN;
	ctx.req.headers.setup_token = token;
	request_body("{\"password\":\"" PASSWORD "\"}");
	dispatch(&web_api_v1_router);
	zassert_equal(ctx.rsp.status, 201, "%s", body);
	set_cookie = response_header("Set-Cookie");
	semi = strchr(set_cookie, ';');
	memcpy(fw_cookie, set_cookie, semi - set_cookie);
	fw_cookie[semi - set_cookie] = '\0';
	zassert_true(json_string("csrf_token", fw_csrf, sizeof(fw_csrf)));
}

static void firmware_reset(void)
{
	const struct flash_area *fa;

	/* A test that failed while holding the worker must not hold the next one. */
	release_worker();
	k_msleep(5);
	if (fw_mounted) {
		zassert_ok(fs_unmount(&fw_mnt));
		fw_mounted = false;
	}
	zassert_ok(flash_area_open(PARTITION_ID(fw_test_partition), &fa));
	zassert_ok(flash_area_erase(fa, 0, fa->fa_size));
	flash_area_close(fa);
	zassert_ok(fs_mount(&fw_mnt));
	fw_mounted = true;
	fw_now = 5000;
	zassert_ok(fw_store_init(DIR, fw_now));
	web_api_v1_set_firmware(&fw_hooks);
	sign_in();
}

static void authorised(enum web_api_method method, const char *path, const char *key)
{
	request(method, path);
	ctx.req.headers.cookie = fw_cookie;
	ctx.req.headers.csrf_token = fw_csrf;
	ctx.req.headers.idempotency_key = key;
}

static void get(const char *path)
{
	request(WEB_API_GET, path);
	ctx.req.headers.cookie = fw_cookie;
	dispatch(&web_api_v1_router);
}

static void expect(uint16_t status, const char *code)
{
	char fragment[64];

	zassert_equal(ctx.rsp.status, status, "status %u: %s", ctx.rsp.status, body);
	if (code != NULL) {
		snprintf(fragment, sizeof(fragment), "\"code\":\"%s\"", code);
		zassert_true(body_has(fragment), "expected %s: %s", code, body);
	}
}

static void sha256_hex_of(const uint8_t *data, size_t len, char *out)
{
	uint8_t digest[32];
	size_t n;

	zassert_ok(psa_crypto_init());
	zassert_ok(psa_hash_compute(PSA_ALG_SHA_256, data, len, digest, sizeof(digest), &n));
	for (size_t i = 0; i < 32; i++) {
		snprintf(&out[2 * i], 3, "%02x", digest[i]);
	}
}

static void create(const char *key, size_t size, const char *sha)
{
	char json[256];

	snprintf(json, sizeof(json), "{\"filename\":\"cp.bin\",\"size_bytes\":%zu,\"sha256\":\"%s\"}",
		 size, sha);
	authorised(WEB_API_POST, "/api/v1/firmware/uploads", key);
	request_body(json);
	dispatch(&web_api_v1_router);
}

/* Create an upload of @p data and return its id in @p id. */
static void create_for(const uint8_t *data, size_t len, const char *key, char *id)
{
	char sha[65];

	sha256_hex_of(data, len, sha);
	create(key, len, sha);
	expect(201, NULL);
	zassert_true(json_string("id", id, FW_STORE_ID_LEN + 1), "%s", body);
}

static void put_chunk(const char *id, const char *offset, const uint8_t *data, size_t len,
		      const char *key)
{
	char path[128];

	snprintf(path, sizeof(path), "/api/v1/firmware/uploads/%s/data%s%s", id,
		 offset != NULL ? "?offset=" : "", offset != NULL ? offset : "");
	authorised(WEB_API_PUT, path, key);
	request_octets(data, len, "application/octet-stream");
	dispatch(&web_api_v1_router);
}

static void post_empty(const char *path, const char *key)
{
	authorised(WEB_API_POST, path, key);
	request_body("{}");
	dispatch(&web_api_v1_router);
}

static void verify(const char *id, const char *key)
{
	char path[96];

	snprintf(path, sizeof(path), "/api/v1/firmware/uploads/%s/verify", id);
	post_empty(path, key);
}

static void cancel(const char *job_id, const char *key)
{
	char path[96];

	snprintf(path, sizeof(path), "/api/v1/jobs/%s/cancel", job_id);
	post_empty(path, key);
}

static void delete_upload(const char *id, const char *key)
{
	char path[96];

	snprintf(path, sizeof(path), "/api/v1/firmware/uploads/%s", id);
	authorised(WEB_API_DELETE, path, key);
	dispatch(&web_api_v1_router);
}

static void get_upload(const char *id)
{
	char path[96];

	snprintf(path, sizeof(path), "/api/v1/firmware/uploads/%s", id);
	get(path);
}

static void accepted_job(char *job_id)
{
	expect(202, NULL);
	zassert_true(json_string("job_id", job_id, JOB_ID_MAX_LEN + 1), "%s", body);
}

static struct job_snapshot wait_job(const char *job_id)
{
	struct job_snapshot job;

	for (int i = 0; i < 2500; i++) {
		zassert_ok(job_get(job_id, &job));
		if (job_state_is_terminal(job.state)) {
			return job;
		}
		k_msleep(2);
	}
	zassert_unreachable("job %s never finished", job_id);
	return job;
}

/* Upload all of @p data in chunks; each chunk's job must succeed. */
static void upload_all(const char *id, const uint8_t *data, size_t len)
{
	for (size_t off = 0; off < len; off += CHUNK) {
		char offset[16];
		char key[32];
		char job_id[JOB_ID_MAX_LEN + 1];
		const size_t n = MIN(CHUNK, len - off);
		struct job_snapshot job;

		snprintf(offset, sizeof(offset), "%zu", off);
		snprintf(key, sizeof(key), "chunk-key-%08zu", off);
		put_chunk(id, offset, &data[off], n, key);
		accepted_job(job_id);
		job = wait_job(job_id);
		zassert_equal(job.state, JOB_STATE_SUCCEEDED, "chunk at %zu: state %d error %s", off,
			      job.state, job.has_error ? job.error.code : "-");
	}
}

static void wait_upload_idle(const char *id)
{
	for (int i = 0; i < 2500; i++) {
		get_upload(id);
		if (body_has("\"active_job_id\":null")) {
			return;
		}
		k_msleep(2);
	}
	zassert_unreachable("upload %s kept its job", id);
}

/* -- not open ------------------------------------------------------------------------- */

ZTEST(v1, test_firmware_bindings_answer_503_until_the_store_is_open)
{
	static const uint8_t byte = 1;
	char sha[65];

	sign_in();
	sha256_hex_of(fx_bare_app, sizeof(fx_bare_app), sha);
	create("create-key-0001x", sizeof(fx_bare_app), sha);
	expect(503, "service_not_ready");
	get("/api/v1/firmware/uploads/upload_0000000000000001");
	expect(503, "service_not_ready");
	put_chunk("upload_0000000000000001", "0", &byte, 1, "chunk-key-0001xx");
	expect(503, "service_not_ready");
	verify("upload_0000000000000001", "verify-key-0001x");
	expect(503, "service_not_ready");
	delete_upload("upload_0000000000000001", "delete-key-0001x");
	expect(503, "service_not_ready");
	/* The list only has nothing to show. */
	get("/api/v1/firmware/uploads");
	expect(200, NULL);
	zassert_true(body_has("{\"uploads\":[]}"), "%s", body);
}

/* -- createUpload / getUpload --------------------------------------------------------- */

ZTEST(v1, test_create_and_get_an_upload)
{
	char id[FW_STORE_ID_LEN + 1];
	char sha[65];
	char location[96];

	firmware_reset();
	sha256_hex_of(fx_valid, sizeof(fx_valid), sha);
	create("create-key-0001x", sizeof(fx_valid), sha);
	expect(201, NULL);
	zassert_true(json_string("id", id, sizeof(id)));
	snprintf(location, sizeof(location), "/api/v1/firmware/uploads/%s", id);
	zassert_str_equal(response_header("Location"), location);
	zassert_true(body_has("\"filename\":\"cp.bin\""), "%s", body);
	zassert_true(body_has("\"received_bytes\":0"));
	zassert_true(body_has("\"state\":\"receiving\""));
	zassert_true(body_has("\"active_job_id\":null"));
	zassert_true(body_has("\"image\":null"));
	zassert_true(body_has("\"error\":null"));
	zassert_true(strstr(body, sha) != NULL, "the declared digest: %s", body);

	get_upload(id);
	expect(200, NULL);
	zassert_true(body_has("\"state\":\"receiving\""), "%s", body);

	get("/api/v1/firmware/uploads/upload_00000000000000ff");
	expect(404, "not_found");
}

/* listUploads: a page finds the upload it did not start; busy names it. */
ZTEST(v1, test_list_uploads_shows_the_staged_upload)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char sha[65];

	firmware_reset();
	get("/api/v1/firmware/uploads");
	expect(200, NULL);
	zassert_true(body_has("{\"uploads\":[]}"), "%s", body);

	sha256_hex_of(fx_valid, sizeof(fx_valid), sha);
	create("create-key-0001x", sizeof(fx_valid), sha);
	expect(201, NULL);
	zassert_true(json_string("id", id, sizeof(id)));

	get("/api/v1/firmware/uploads");
	expect(200, NULL);
	zassert_true(body_has("{\"uploads\":[{\"id\":\""), "%s", body);
	zassert_true(strstr(body, id) != NULL, "%s", body);
	zassert_true(body_has("\"target\":\"esp32c6\""), "%s", body);
	zassert_true(body_has("\"state\":\"receiving\""), "%s", body);

	create("create-key-0002x", sizeof(fx_valid), sha);
	expect(409, "busy");
	zassert_true(strstr(body, id) != NULL, "busy names the upload: %s", body);

	delete_upload(id, "delete-key-0001x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	get("/api/v1/firmware/uploads");
	expect(200, NULL);
	zassert_true(body_has("{\"uploads\":[]}"), "%s", body);
}

ZTEST(v1, test_create_upload_replays_by_key)
{
	char id[FW_STORE_ID_LEN + 1];
	char again[FW_STORE_ID_LEN + 1];
	char sha[65];
	char job_id[JOB_ID_MAX_LEN + 1];

	firmware_reset();
	sha256_hex_of(fx_valid, sizeof(fx_valid), sha);
	create("create-key-0001x", sizeof(fx_valid), sha);
	expect(201, NULL);
	zassert_true(json_string("id", id, sizeof(id)));

	/* The same request again is the same upload, not busy and not a second one. */
	create("create-key-0001x", sizeof(fx_valid), sha);
	expect(201, NULL);
	zassert_true(json_string("id", again, sizeof(again)));
	zassert_str_equal(again, id);

	create("create-key-0001x", sizeof(fx_valid) + 1, sha);
	expect(409, "idempotency_conflict");

	/* A new request while one is staged. */
	create("create-key-0002x", sizeof(fx_valid), sha);
	expect(409, "busy");

	/* Deleted meanwhile: the replay says it is gone. */
	delete_upload(id, "delete-key-0001x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	create("create-key-0001x", sizeof(fx_valid), sha);
	expect(404, "not_found");
}

ZTEST(v1, test_create_upload_refusals)
{
	char sha[65];

	firmware_reset();
	sha256_hex_of(fx_valid, sizeof(fx_valid), sha);

	create("create-key-0001x", CONFIG_FIRMWARE_STORE_MAX_BYTES + 1, sha);
	expect(413, "payload_too_large");
	create("create-key-0002x", 5000000000LL, sha);
	expect(413, "payload_too_large");
	/* Allowed by the slot, not by this 1 MiB volume. */
	create("create-key-0003x", CONFIG_FIRMWARE_STORE_MAX_BYTES, sha);
	expect(507, "storage_full");
	create("create-key-0004x", 0, sha);
	expect(422, "validation_failed");
	create("create-key-0005x", 10, "ABCDEF");
	expect(422, "validation_failed");
	zassert_true(body_has("\"path\":\"/sha256\""), "%s", body);
	/* Upper-case hex is not the schema's pattern. */
	for (size_t i = 0; i < 64; i++) {
		if (sha[i] >= 'a' && sha[i] <= 'f') {
			sha[i] = sha[i] - 'a' + 'A';
			break;
		}
	}
	create("create-key-0006x", 10, sha);
	expect(422, "validation_failed");
}

/* -- chunks and the check, end to end -------------------------------------------------- */

ZTEST(v1, test_upload_and_verify_a_valid_merged_file)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char url[96];
	char size[48];

	firmware_reset();
	create_for(fx_valid, sizeof(fx_valid), "create-key-0001x", id);
	upload_all(id, fx_valid, sizeof(fx_valid));

	get_upload(id);
	expect(200, NULL);
	snprintf(size, sizeof(size), "\"received_bytes\":%zu", sizeof(fx_valid));
	zassert_true(body_has(size), "%s", body);
	zassert_true(body_has("\"state\":\"receiving\""), "%s", body);

	verify(id, "verify-key-0001x");
	accepted_job(job_id);
	snprintf(url, sizeof(url), "\"resource_url\":\"/api/v1/firmware/uploads/%s\"", id);
	zassert_true(body_has(url), "%s", body);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	wait_upload_idle(id);

	get_upload(id);
	expect(200, NULL);
	zassert_true(body_has("\"state\":\"ready\""), "%s", body);
	zassert_true(body_has("\"image\":{\"format\":\"raw_full_flash\",\"format_version\":null,"
			      "\"target\":\"esp32c6\",\"version\":"),
		     "%s", body);
	zassert_true(body_has("\"kind\":\"recovery_bundle\",\"partition_layout_id\":"
			      "\"cedar-c6-ota-4m-2x1792k\",\"host_protocol\":\"esp-hosted-mcu-3\","
			      "\"signature_verified\":null,\"allowed_methods\":[\"uart\"]}"),
		     "%s", body);
	zassert_true(body_has("\"error\":null"), "%s", body);

	/* The job points at its upload. */
	get_upload(id);
	snprintf(url, sizeof(url), "/api/v1/jobs/%s", job_id);
	get(url);
	snprintf(url, sizeof(url), "\"resource_url\":\"/api/v1/firmware/uploads/%s\"", id);
	zassert_true(body_has(url), "%s", body);
	zassert_true(body_has("\"kind\":\"firmware_verify\""), "%s", body);

	/* A checked upload takes no more data and no second check. */
	put_chunk(id, "0", fx_valid, 16, "chunk-key-after-x");
	expect(409, "invalid_state");
	verify(id, "verify-key-0002x");
	expect(409, "invalid_state");
}

ZTEST(v1, test_a_bare_application_fails_its_check)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	struct job_snapshot job;

	firmware_reset();
	create_for(fx_bare_app, sizeof(fx_bare_app), "create-key-0001x", id);
	upload_all(id, fx_bare_app, sizeof(fx_bare_app));
	verify(id, "verify-key-0001x");
	accepted_job(job_id);
	job = wait_job(job_id);
	zassert_equal(job.state, JOB_STATE_FAILED);
	zassert_str_equal(job.error.code, "invalid_image");
	wait_upload_idle(id);

	get_upload(id);
	zassert_true(body_has("\"state\":\"failed\""), "%s", body);
	zassert_true(body_has("\"image\":null"), "%s", body);
	zassert_true(body_has("\"error\":{\"code\":\"invalid_image\",\"message\":\"This is an "
			      "application image"),
		     "%s", body);
	zassert_true(body_has("\"retryable\":false}"), "%s", body);

	/* A failed upload is replaced by a new one. */
	create_for(fx_valid, sizeof(fx_valid), "create-key-0002x", id);
}

ZTEST(v1, test_chunk_refusals)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];

	firmware_reset();
	create_for(fx_bare_app, sizeof(fx_bare_app), "create-key-0001x", id);

	/* offset: required, decimal, unsigned, 32 bits. */
	put_chunk(id, NULL, fx_bare_app, 16, "chunk-key-0001xx");
	expect(400, "invalid_query");
	put_chunk(id, "", fx_bare_app, 16, "chunk-key-0002xx");
	expect(400, "invalid_query");
	put_chunk(id, "abc", fx_bare_app, 16, "chunk-key-0003xx");
	expect(400, "invalid_query");
	put_chunk(id, "-1", fx_bare_app, 16, "chunk-key-0004xx");
	expect(400, "invalid_query");
	put_chunk(id, "%2B0", fx_bare_app, 16, "chunk-key-0005xx");
	expect(400, "invalid_query");
	put_chunk(id, "4294967296", fx_bare_app, 16, "chunk-key-0006xx");
	expect(400, "invalid_query");
	put_chunk(id, "12345678901", fx_bare_app, 16, "chunk-key-0007xx");
	expect(400, "invalid_query");

	put_chunk(id, "16", fx_bare_app, 16, "chunk-key-0008xx");
	expect(409, "offset_mismatch");
	zassert_true(body_has("The next acceptable offset is 0"), "%s", body);
	put_chunk(id, "4294967295", fx_bare_app, 16, "chunk-key-0009xx");
	expect(409, "offset_mismatch");
	put_chunk("upload_00000000000000ff", "0", fx_bare_app, 16, "chunk-key-0010xx");
	expect(404, "not_found");
	put_chunk(id, "0", fx_valid, 2000, "chunk-key-0011xx");
	expect(422, "validation_failed");
	zassert_true(body_has("past the declared size"), "%s", body);

	/* One chunk at a time: the second is refused while the first is not on storage. */
	hold_worker();
	put_chunk(id, "0", fx_bare_app, 16, "chunk-key-0012xx");
	accepted_job(job_id);
	put_chunk(id, "16", &fx_bare_app[16], 16, "chunk-key-0013xx");
	expect(409, "busy");
	get_upload(id);
	zassert_true(body_has("\"received_bytes\":0"), "not before the job: %s", body);
	release_worker();
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	get_upload(id);
	zassert_true(body_has("\"received_bytes\":16"), "%s", body);
	zassert_true(body_has("\"active_job_id\":null"), "%s", body);
	put_chunk(id, "16", &fx_bare_app[16], 16, "chunk-key-0014xx");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
}

ZTEST(v1, test_chunk_replay_is_its_job_and_writes_once)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char again[JOB_ID_MAX_LEN + 1];
	char url[96];
	uint8_t other[16];

	firmware_reset();
	create_for(fx_bare_app, sizeof(fx_bare_app), "create-key-0001x", id);
	put_chunk(id, "0", fx_bare_app, 16, "chunk-key-0001xx");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);

	put_chunk(id, "0", fx_bare_app, 16, "chunk-key-0001xx");
	accepted_job(again);
	zassert_str_equal(again, job_id);
	get_upload(id);
	zassert_true(body_has("\"received_bytes\":16"), "written once: %s", body);

	memcpy(other, fx_bare_app, sizeof(other));
	other[3] ^= 0x55;
	put_chunk(id, "0", other, sizeof(other), "chunk-key-0001xx");
	expect(409, "idempotency_conflict");

	snprintf(url, sizeof(url), "/api/v1/jobs/%s", job_id);
	get(url);
	expect(200, NULL);
	zassert_true(body_has("\"kind\":\"upload_chunk\""), "%s", body);
	zassert_true(body_has("\"progress\":{\"completed\":16,\"total\":16,\"unit\":\"bytes\"}"),
		     "%s", body);
	snprintf(url, sizeof(url), "\"resource_url\":\"/api/v1/firmware/uploads/%s\"", id);
	zassert_true(body_has(url), "%s", body);
}

ZTEST(v1, test_a_chunk_the_volume_cannot_take_fails_its_job)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	static uint8_t filler[4096];
	struct fs_file_t f;
	struct job_snapshot job;

	firmware_reset();
	create_for(fx_valid, sizeof(fx_valid), "create-key-0001x", id);

	/* Another writer fills the volume after the upload was accepted - with a
	 * file that fits, synced: one that runs out of space is dropped by
	 * LittleFS on close and frees everything again. 800 KiB leaves less than
	 * the chunk's missing bytes plus the store's reserve. */
	memset(filler, 0xa5, sizeof(filler));
	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, MNT "/filler", FS_O_CREATE | FS_O_WRITE));
	size_t written = 0;
	ssize_t n = 0;

	while (written < 800U * 1024U) {
		n = fs_write(&f, filler, sizeof(filler));
		zassert_equal(n, (ssize_t)sizeof(filler), "filler write at %zu: %zd", written, n);
		written += sizeof(filler);
	}
	zassert_ok(fs_sync(&f));
	zassert_ok(fs_close(&f));
	struct fs_statvfs sv;

	zassert_ok(fs_statvfs(MNT, &sv));

	put_chunk(id, "0", fx_valid, CHUNK, "chunk-key-0001xx");
	accepted_job(job_id);
	job = wait_job(job_id);
	zassert_equal(job.state, JOB_STATE_FAILED,
		      "filler %zu bytes (last write %zd), then %lu of %lu blocks of %lu free; job %d",
		      written, n, (unsigned long)sv.f_bfree, (unsigned long)sv.f_blocks,
		      (unsigned long)sv.f_frsize, job.state);
	zassert_str_equal(job.error.code, "storage_full");
	get_upload(id);
	zassert_true(body_has("\"received_bytes\":0"), "the offset did not move: %s", body);
	zassert_true(body_has("\"active_job_id\":null"), "%s", body);
}

ZTEST(v1, test_verify_refusals_and_replay)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char again[JOB_ID_MAX_LEN + 1];

	firmware_reset();
	create_for(fx_bare_app, sizeof(fx_bare_app), "create-key-0001x", id);
	verify(id, "verify-key-0001x");
	expect(409, "invalid_state");
	verify("upload_00000000000000ff", "verify-key-0002x");
	expect(404, "not_found");

	hold_worker();
	put_chunk(id, "0", fx_bare_app, sizeof(fx_bare_app), "chunk-key-0001xx");
	accepted_job(job_id);
	verify(id, "verify-key-0003x");
	expect(409, "busy");
	release_worker();
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);

	verify(id, "verify-key-0004x");
	accepted_job(job_id);
	verify(id, "verify-key-0004x");
	accepted_job(again);
	zassert_str_equal(again, job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_FAILED);
	/* Refused after the claim was used: the failed check can run again. */
	wait_upload_idle(id);
	verify(id, "verify-key-0005x");
	accepted_job(again);
	zassert_not_equal(strcmp(again, job_id), 0);
	zassert_equal(wait_job(again).state, JOB_STATE_FAILED);
}

/* -- cancelJob --------------------------------------------------------------------------- */

ZTEST(v1, test_cancel_a_check)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char again[JOB_ID_MAX_LEN + 1];

	firmware_reset();
	create_for(fx_valid, sizeof(fx_valid), "create-key-0001x", id);
	upload_all(id, fx_valid, sizeof(fx_valid));

	hold_worker();
	verify(id, "verify-key-0001x");
	accepted_job(job_id);
	get_upload(id);
	zassert_true(body_has("\"state\":\"verifying\""), "%s", body);
	cancel(job_id, "cancel-key-0001x");
	accepted_job(again);
	zassert_str_equal(again, job_id);
	/* The same request again is answered the same way. */
	cancel(job_id, "cancel-key-0001x");
	accepted_job(again);
	zassert_str_equal(again, job_id);
	/* A new request for a job no longer running. */
	cancel(job_id, "cancel-key-0002x");
	expect(409, "invalid_state");
	release_worker();

	zassert_equal(wait_job(job_id).state, JOB_STATE_CANCELLED);
	wait_upload_idle(id);
	get_upload(id);
	zassert_true(body_has("\"state\":\"receiving\""), "cancelled is not failed: %s", body);
	zassert_true(body_has("\"image\":null"), "%s", body);

	/* It can be checked again. */
	verify(id, "verify-key-0002x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);

	cancel("job_does_not_exist", "cancel-key-0003x");
	expect(404, "not_found");
}

ZTEST(v1, test_a_chunk_cannot_be_cancelled)
{
	char id[FW_STORE_ID_LEN + 1];
	char chunk_job[JOB_ID_MAX_LEN + 1];

	firmware_reset();
	create_for(fx_bare_app, sizeof(fx_bare_app), "create-key-0002x", id);
	hold_worker();
	put_chunk(id, "0", fx_bare_app, 16, "chunk-key-0001xx");
	accepted_job(chunk_job);
	cancel(chunk_job, "cancel-key-0004x");
	expect(409, "invalid_state");
	release_worker();
	zassert_equal(wait_job(chunk_job).state, JOB_STATE_SUCCEEDED);
}

ZTEST(v1, test_cancel_rules_for_jobs_that_are_not_firmware)
{
	struct job_snapshot job;
	char job_id[JOB_ID_MAX_LEN + 1];
	char url[64];
	const struct job_create_params apply = {.kind = JOB_KIND_NETWORK_APPLY, .cancellable = true};
	const struct job_create_params update = {.kind = JOB_KIND_COPROCESSOR_UPDATE,
						 .cancellable = true};

	sign_in();
	/* A network apply is cancelled through its transaction, even when job-manager
	 * would allow it. */
	zassert_equal(job_create(&apply, &job), JOB_CREATE_NEW);
	cancel(job.id, "cancel-key-apply1");
	expect(409, "invalid_state");
	zassert_true(body_has("through DELETE on its transaction"), "%s", body);
	zassert_ok(job_get(job.id, &job));
	zassert_false(job_state_is_terminal(job.state));

	/* An install before `begin` is cancellable, and points at the coprocessor. */
	zassert_equal(job_create(&update, &job), JOB_CREATE_NEW);
	cancel(job.id, "cancel-key-update");
	accepted_job(job_id);
	zassert_true(body_has("\"resource_url\":\"/api/v1/coprocessor/status\""), "%s", body);
	snprintf(url, sizeof(url), "/api/v1/jobs/%s", job.id);
	get(url);
	zassert_true(body_has("\"state\":\"cancelled\""), "%s", body);
	zassert_true(body_has("\"resource_url\":\"/api/v1/coprocessor/status\""), "%s", body);
}

/* -- deleteUpload ----------------------------------------------------------------------- */

ZTEST(v1, test_delete_an_upload)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char again[JOB_ID_MAX_LEN + 1];

	firmware_reset();
	create_for(fx_bare_app, sizeof(fx_bare_app), "create-key-0001x", id);

	/* Busy while a chunk is pending, refused before any job exists. */
	hold_worker();
	put_chunk(id, "0", fx_bare_app, 16, "chunk-key-0001xx");
	accepted_job(job_id);
	delete_upload(id, "delete-key-0001x");
	expect(409, "busy");
	release_worker();
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);

	put_chunk(id, "16", &fx_bare_app[16], sizeof(fx_bare_app) - 16, "chunk-key-0002xx");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);

	delete_upload(id, "delete-key-0002x");
	accepted_job(job_id);
	delete_upload(id, "delete-key-0002x");
	accepted_job(again);
	zassert_str_equal(again, job_id);
	zassert_true(body_has("\"resource_url\":null"), "%s", body);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	get_upload(id);
	expect(404, "not_found");
	delete_upload(id, "delete-key-0003x");
	expect(404, "not_found");
}

ZTEST(v1, test_an_install_holds_the_upload)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];

	firmware_reset();
	create_for(fx_valid, sizeof(fx_valid), "create-key-0001x", id);
	upload_all(id, fx_valid, sizeof(fx_valid));
	verify(id, "verify-key-0001x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	wait_upload_idle(id);

	zassert_ok(fw_store_set_in_use(id, true));
	delete_upload(id, "delete-key-0001x");
	expect(409, "busy");
	verify(id, "verify-key-0002x");
	expect(409, "busy");
	{
		char sha[65];

		sha256_hex_of(fx_bare_app, sizeof(fx_bare_app), sha);
		create("create-key-0002x", sizeof(fx_bare_app), sha);
		expect(409, "busy");
	}
	zassert_ok(fw_store_set_in_use(id, false));
	delete_upload(id, "delete-key-0002x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
}

/* -- the sha256 member's format --------------------------------------------------------- */

ZTEST(v1, test_create_upload_sha256_is_64_lowercase_hex_digits)
{
	char sha[80];

	firmware_reset();
	/* 65 digits. */
	memset(sha, 'a', 65);
	sha[65] = '\0';
	create("create-key-0101x", 10, sha);
	expect(422, "validation_failed");
	/* 64 characters, one of them a letter past f. */
	memset(sha, 'a', 64);
	sha[40] = 'g';
	sha[64] = '\0';
	create("create-key-0102x", 10, sha);
	expect(422, "validation_failed");
	memset(sha, 'a', 64);
	sha[63] = 'z';
	create("create-key-0103x", 10, sha);
	expect(422, "validation_failed");
	/* 64 good digits are accepted. */
	memset(sha, '0', 64);
	sha[63] = 'f';
	create("create-key-0104x", sizeof(fx_bare_app), sha);
	expect(201, NULL);
}

/* -- startCoprocessorUpdate ------------------------------------------------------------- */

static bool over_ethernet;

static bool fake_over_ethernet(const struct web_auth_peer *local)
{
	ARG_UNUSED(local);
	return over_ethernet;
}

static const struct web_api_v1_coprocessor update_hooks = {
	.request_over_ethernet = fake_over_ethernet,
};

/* The updater's image is firmware-store's, held while the install reads it -
 * as the board's platform does. */
static char image_upload[FW_STORE_ID_LEN + 1];

static int img_open(void *c, const char *upload_id, uint32_t *size)
{
	int rc = fw_store_image_open(upload_id, size);

	ARG_UNUSED(c);
	if (rc == 0) {
		rc = fw_store_set_in_use(upload_id, true);
		if (rc != 0) {
			fw_store_image_close();
			return rc;
		}
		snprintf(image_upload, sizeof(image_upload), "%s", upload_id);
	}
	return rc;
}

static int img_read(void *c, uint32_t offset, uint8_t *buf, size_t len)
{
	ARG_UNUSED(c);
	return fw_store_image_read(offset, buf, len);
}

static void img_close(void *c)
{
	ARG_UNUSED(c);
	fw_store_image_close();
	(void)fw_store_set_in_use(image_upload, false);
}

static struct coprocessor_updater_platform store_platform;

void v1_update_fake_reset(void)
{
	fake_update_init();
	store_platform = fake_update_platform;
	store_platform.image_open = img_open;
	store_platform.image_read = img_read;
	store_platform.image_close = img_close;
	/* -EBUSY when a failed test left an install running: its test failed already. */
	(void)coprocessor_updater_init(&store_platform);
	over_ethernet = true;
	web_api_v1_set_coprocessor(&update_hooks);
}

static void post_update(const char *upload_id, const char *method, bool acknowledge,
			const char *key)
{
	char json[200];

	snprintf(json, sizeof(json),
		 "{\"upload_id\":\"%s\",\"method\":\"%s\",\"acknowledge_recovery\":%s}", upload_id,
		 method, acknowledge ? "true" : "false");
	authorised(WEB_API_POST, "/api/v1/coprocessor/updates", key);
	request_body(json);
	dispatch(&web_api_v1_router);
}

/* A verified merged file, ready to install; its id in @p id. */
static void ready_upload(char *id)
{
	char job_id[JOB_ID_MAX_LEN + 1];

	create_for(fx_valid, sizeof(fx_valid), "create-key-0201x", id);
	upload_all(id, fx_valid, sizeof(fx_valid));
	verify(id, "verify-key-0201x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	wait_upload_idle(id);
}

static void wait_updater_idle(void)
{
	struct coprocessor_updater_state st;

	for (int i = 0; i < 2500; i++) {
		coprocessor_updater_get_state(&st);
		if (!st.active) {
			return;
		}
		k_msleep(2);
	}
	zassert_unreachable("the install never ended");
}

/* A coprocessor-manager whose console cannot attach: the UART is `unavailable`. */
static int broken_attach(void *c, enum coprocessor_uart_mode owner)
{
	ARG_UNUSED(c);
	ARG_UNUSED(owner);
	return -EIO;
}

static int broken_detach(void *c)
{
	ARG_UNUSED(c);
	return 0;
}

static uint32_t broken_activity(void *c)
{
	ARG_UNUSED(c);
	return 0;
}

static int broken_reset(void *c, bool download)
{
	ARG_UNUSED(c);
	ARG_UNUSED(download);
	return 0;
}

static void broken_marker(void *c, enum log_store_kind kind, uint32_t generation, const char *text)
{
	ARG_UNUSED(c);
	ARG_UNUSED(kind);
	ARG_UNUSED(generation);
	ARG_UNUSED(text);
}

static int64_t broken_now(void *c)
{
	ARG_UNUSED(c);
	return fw_now;
}

static void broken_sleep(void *c, uint32_t ms)
{
	ARG_UNUSED(c);
	fw_now += ms;
}

static const struct coprocessor_platform broken_uart = {
	.uart_attach = broken_attach,
	.uart_detach = broken_detach,
	.uart_rx_activity = broken_activity,
	.c6_reset = broken_reset,
	.marker = broken_marker,
	.now_ms = broken_now,
	.sleep_ms = broken_sleep,
};

ZTEST(v1, test_update_refusals_come_in_the_contracts_order)
{
	char id[FW_STORE_ID_LEN + 1];

	firmware_reset();
	create_for(fx_valid, sizeof(fx_valid), "create-key-0301x", id);

	/* ota before everything, even a request not over Ethernet. */
	over_ethernet = false;
	post_update("upload_00000000000000ff", "ota", false, "update-key-0001x");
	expect(503, "capability_unavailable");
	post_update("upload_00000000000000ff", "uart", false, "update-key-0002x");
	expect(409, "ethernet_required");
	/* No hook at all is no proof of Ethernet either. */
	over_ethernet = true;
	web_api_v1_set_coprocessor(NULL);
	post_update(id, "uart", true, "update-key-0003x");
	expect(409, "ethernet_required");
	web_api_v1_set_coprocessor(&update_hooks);

	zassert_ok(coprocessor_manager_set_mode(COPROCESSOR_UART_USB_BRIDGE));
	post_update("upload_00000000000000ff", "uart", false, "update-key-0004x");
	expect(404, "not_found");
	/* Not ready, not acknowledged, bridged: not ready first. */
	post_update(id, "uart", false, "update-key-0005x");
	expect(409, "invalid_state");

	upload_all(id, fx_valid, sizeof(fx_valid));
	{
		char job_id[JOB_ID_MAX_LEN + 1];

		verify(id, "verify-key-0301x");
		accepted_job(job_id);
		zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
		wait_upload_idle(id);
	}
	/* Ready, not acknowledged, bridged: the acknowledgement first. */
	post_update(id, "uart", false, "update-key-0006x");
	expect(422, "validation_failed");
	zassert_true(body_has("acknowledge_recovery"), "%s", body);
	post_update(id, "uart", true, "update-key-0007x");
	expect(409, "busy");
	zassert_true(body_has("bridged to USB"), "%s", body);
	zassert_ok(coprocessor_manager_set_mode(COPROCESSOR_UART_CONSOLE));

	zassert_equal(coprocessor_manager_init(&broken_uart), -EIO);
	post_update(id, "uart", true, "update-key-0008x");
	expect(503, "capability_unavailable");
	get("/api/v1/coprocessor/status");
	zassert_true(body_has("\"uart_update\":{\"available\":false,\"reason\":\"uart_unavailable\"}"),
		     "%s", body);
	v1_coprocessor_fake_init();
	web_api_v1_set_coprocessor(&update_hooks);

	/* Nothing above created a job or started an install. */
	zassert_equal(job_active_count(), 0U);
	get("/api/v1/coprocessor/status");
	zassert_true(body_has("\"last_update\":null"), "%s", body);

	/* Without the store the install cannot find its file. */
	web_api_v1_set_firmware(NULL);
	post_update(id, "uart", true, "update-key-0009x");
	expect(503, "service_not_ready");
}

ZTEST(v1, test_an_install_runs_to_succeeded_on_the_worker)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char again[JOB_ID_MAX_LEN + 1];
	char fragment[192];
	struct job_snapshot job;

	firmware_reset();
	ready_upload(id);
	post_update(id, "uart", true, "update-key-0101x");
	accepted_job(job_id);
	zassert_true(body_has("\"resource_url\":\"/api/v1/coprocessor/status\""), "%s", body);
	zassert_true(response_header("Location") != NULL);

	/* The same request again is the same install, not busy. */
	post_update(id, "uart", true, "update-key-0101x");
	accepted_job(again);
	zassert_str_equal(again, job_id);
	post_update(id, "uart", false, "update-key-0101x");
	expect(409, "idempotency_conflict");

	job = wait_job(job_id);
	zassert_equal(job.state, JOB_STATE_SUCCEEDED, "state %d error %s", job.state,
		      job.has_error ? job.error.code : "-");
	wait_updater_idle();
	zassert_equal(fake_update.written, sizeof(fx_valid), "the whole file went to the chip");
	zassert_equal(fake_update.begin_size, sizeof(fx_valid));

	get("/api/v1/coprocessor/status");
	expect(200, NULL);
	snprintf(fragment, sizeof(fragment), "\"last_update\":{\"job_id\":\"%s\",\"state\":\"succeeded\","
		 "\"method\":\"uart\",\"version\":", job_id);
	zassert_true(body_has(fragment), "%s", body);
	zassert_false(body_has("\"version\":null"), "a confirmed version: %s", body);
	zassert_true(body_has("\"recovery_required\":false,\"error\":null}"), "%s", body);
	zassert_false(body_has("\"state\":\"updating\""), "%s", body);
	zassert_true(body_has("\"uart_update\":{\"available\":true,\"reason\":null}"), "%s", body);

	snprintf(fragment, sizeof(fragment), "/api/v1/jobs/%s", job_id);
	get(fragment);
	zassert_true(body_has("\"kind\":\"coprocessor_update\""), "%s", body);
	zassert_true(body_has("\"resource_url\":\"/api/v1/coprocessor/status\""), "%s", body);

	/* The install let go of the file. */
	delete_upload(id, "delete-key-0101x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
}

ZTEST(v1, test_an_install_cancelled_before_begin_changes_nothing)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char again[JOB_ID_MAX_LEN + 1];

	firmware_reset();
	ready_upload(id);
	hold_worker();
	post_update(id, "uart", true, "update-key-0201x");
	accepted_job(job_id);
	get("/api/v1/coprocessor/status");
	zassert_true(body_has("\"state\":\"updating\""), "accepted is updating: %s", body);
	zassert_true(body_has("\"uart_update\":{\"available\":false,\"reason\":\"uart_flashing\"}"),
		     "%s", body);
	get("/api/v1/capabilities");
	zassert_true(body_has("\"esp32_uart\":{\"available\":false,\"reason\":\"uart_flashing\"}"),
		     "%s", body);
	/* A second install, even unacknowledged, is busy. */
	post_update(id, "uart", false, "update-key-0202x");
	expect(409, "busy");

	cancel(job_id, "cancel-key-0201x");
	accepted_job(again);
	zassert_str_equal(again, job_id);
	release_worker();
	zassert_equal(wait_job(job_id).state, JOB_STATE_CANCELLED);
	wait_updater_idle();
	zassert_equal(fake_update.count[FUP_BEGIN], 0, "the chip was not erased");
	get("/api/v1/coprocessor/status");
	zassert_true(body_has("\"last_update\":null"), "%s", body);
	delete_upload(id, "delete-key-0201x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
}

static K_SEM_DEFINE(in_write, 0, 1);
static K_SEM_DEFINE(let_write, 0, 1);

static void hold_in_write(void *arg)
{
	ARG_UNUSED(arg);
	k_sem_give(&in_write);
	(void)k_sem_take(&let_write, K_SECONDS(20));
}

ZTEST(v1, test_an_install_past_begin_cannot_be_cancelled)
{
	char id[FW_STORE_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];

	firmware_reset();
	ready_upload(id);
	k_sem_reset(&in_write);
	k_sem_reset(&let_write);
	fake_update.hook = hold_in_write;
	fake_update.hook_call = FUP_WRITE;
	fake_update.hook_nth = 0;

	post_update(id, "uart", true, "update-key-0301x");
	accepted_job(job_id);
	zassert_ok(k_sem_take(&in_write, K_SECONDS(20)), "the install reached writing");
	cancel(job_id, "cancel-key-0301x");
	expect(409, "invalid_state");
	delete_upload(id, "delete-key-0301x");
	expect(409, "busy");
	k_sem_give(&let_write);

	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	wait_updater_idle();
}

ZTEST(v1, test_an_install_cut_by_a_restart_is_interrupted)
{
	char fragment[128];

	firmware_reset();
	/* The journal says an install was writing when the STM32 went away. */
	fake_update_init();
	fake_update.has_journal = true;
	fake_update.journal.format = COPROCESSOR_UPDATE_JOURNAL_FORMAT;
	fake_update.journal.active = true;
	fake_update.journal.phase = COPROCESSOR_UPDATE_WRITING;
	strcpy(fake_update.journal.job_id, "job_before_restart");
	strcpy(fake_update.journal.upload_id, "upload_0000000000000001");
	zassert_ok(coprocessor_updater_init(&store_platform));

	get("/api/v1/coprocessor/status");
	expect(200, NULL);
	zassert_true(body_has("\"last_update\":{\"job_id\":\"job_before_restart\","
			      "\"state\":\"interrupted\",\"method\":\"uart\",\"version\":null,"
			      "\"recovery_required\":true,\"error\":{\"code\":"),
		     "%s", body);
	snprintf(fragment, sizeof(fragment), "\"request_id\":\"%s\"",
		 response_header("X-Request-ID"));
	zassert_true(body_has(fragment), "the error is an ErrorDetail: %s", body);
	zassert_false(body_has("\"state\":\"updating\""), "nothing continues: %s", body);

	/* Cut before `begin`: nothing on the chip changed. */
	fake_update_init();
	fake_update.has_journal = true;
	fake_update.journal.format = COPROCESSOR_UPDATE_JOURNAL_FORMAT;
	fake_update.journal.active = true;
	fake_update.journal.phase = COPROCESSOR_UPDATE_ENTERING_BOOTLOADER;
	strcpy(fake_update.journal.job_id, "job_before_begin");
	zassert_ok(coprocessor_updater_init(&store_platform));
	get("/api/v1/coprocessor/status");
	zassert_true(body_has("\"job_id\":\"job_before_begin\",\"state\":\"interrupted\""), "%s",
		     body);
	zassert_true(body_has("\"recovery_required\":false"), "%s", body);
}
