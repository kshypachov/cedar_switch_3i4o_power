/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The STM32 update through API v1: the upload operations dispatched by target
 * and by id (firmware.c), getSystemFirmware and startSystemUpdate
 * (system_update.c), capabilities - over the real firmware-store,
 * system-image-store (slot 2 on the sim flash) and system-updater, whose board
 * is the fake of tests/fakes/fake_system_platform with the upload calls going to
 * the real slot store, as src/services/system does on the device.
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
#include <device_config_store/device_config_store.h>
#include <firmware_store/firmware_store.h>
#include <job_manager/job_manager.h>
#include <log_store/log_store.h>
#include <matter_service/matter_service.h>
#include <network_manager/network_manager.h>
#include <system_image_store/system_image_store.h>
#include <system_updater/system_updater.h>

#include "fake_iface.h"
#include "fake_storage.h"
#include "fake_system_platform.h"
#include "harness.h"
#include "v1_internal.h"
#include "web_api_v1.h"

#define PASSWORD   "correct horse battery"
#define LAN_ORIGIN "http://192.168.88.14"
#define MNT        "/fw"
#define DIR        "/fw/firmware"
#define CHUNK      16384U
#define SLOT_SIZE  (4U * 1024U * 1024U)
#define SECTOR     4096U
#define TRAILER    65536U
#define MAX_IMAGE  (SLOT_SIZE - TRAILER)

static const uint8_t img_valid[] = {
#include "img_valid.inc"
};
static const uint8_t img_valid_large[] = {
#include "img_valid_large.inc"
};
static const uint8_t img_bad_magic[] = {
#include "img_bad_magic.inc"
};
static const uint8_t img_wrong_sp[] = {
#include "img_wrong_sp.inc"
};

/* tests/fixtures/stm32/manifest.json */
#define VALID_VERSION "1.2.3+4"
#define VALID_HASH    "ceb5a088a3532bebccf9c3a5f0a20c958a923766c4cab17117fc5528b5659c71"

static const struct web_api_v1_identity identity = {
	.device_id = "cedar-0011aabb",
	.model = "cedar_switch_3in4out_power",
	.firmware_version = "test-firmware",
	.frontend_version = "test-frontend",
	.boot_id = "boot_test1",
};

static char cookie[96];
static char csrf[WEB_AUTH_TOKEN_LEN + 1];

/* -- fakes of the bindings this suite does not drive ------------------------------ */

static void mf_close(void)
{
}

static void mf_read_window(struct matter_window_reading *out)
{
	*out = (struct matter_window_reading){0};
}

static int mf_schedule(void (*fn)(void *arg), void *arg)
{
	ARG_UNUSED(fn);
	ARG_UNUSED(arg);
	return 0;
}

static int64_t mf_now(void)
{
	return fake_now;
}

static int mf_open(uint32_t timeout_seconds)
{
	ARG_UNUSED(timeout_seconds);
	return 0;
}

static int mf_read_codes(struct matter_codes *out)
{
	ARG_UNUSED(out);
	return -ENODATA;
}

static size_t mf_read_fabrics(struct matter_fabric *out, size_t max)
{
	ARG_UNUSED(out);
	ARG_UNUSED(max);
	return 0;
}

static uint32_t mf_min(void)
{
	return 180;
}

static uint32_t mf_max(void)
{
	return 900;
}

static const struct matter_service_platform mf_platform = {
	.schedule = mf_schedule,
	.now_ms = mf_now,
	.open_basic_window = mf_open,
	.close_window = mf_close,
	.read_window = mf_read_window,
	.read_codes = mf_read_codes,
	.read_fabrics = mf_read_fabrics,
	.min_window_seconds = mf_min,
	.max_window_seconds = mf_max,
};

static struct fake_storage net_storage;
static struct device_config_backend net_backend;
static struct fake_net net;
static struct network_iface_ops net_ops;

static int64_t net_clock(void)
{
	return 1000;
}

static void kick(void)
{
}

static const struct web_api_v1_network net_hooks = {.kick = kick};

static int cp_attach(void *c, enum coprocessor_uart_mode owner)
{
	ARG_UNUSED(c);
	ARG_UNUSED(owner);
	return 0;
}

static int cp_detach(void *c)
{
	ARG_UNUSED(c);
	return 0;
}

static uint32_t cp_activity(void *c)
{
	ARG_UNUSED(c);
	return 0;
}

static int cp_reset(void *c, bool download)
{
	ARG_UNUSED(c);
	ARG_UNUSED(download);
	return 0;
}

static bool cp_transport(void *c)
{
	ARG_UNUSED(c);
	return false;
}

static void cp_marker(void *c, enum log_store_kind kind, uint32_t generation, const char *text)
{
	ARG_UNUSED(c);
	ARG_UNUSED(kind);
	ARG_UNUSED(generation);
	ARG_UNUSED(text);
}

static int64_t cp_now(void *c)
{
	ARG_UNUSED(c);
	return fake_now;
}

static void cp_sleep(void *c, uint32_t ms)
{
	ARG_UNUSED(c);
	fake_now += ms;
}

static const struct coprocessor_platform cp_platform = {
	.uart_attach = cp_attach,
	.uart_detach = cp_detach,
	.uart_rx_activity = cp_activity,
	.c6_reset = cp_reset,
	.transport_ready = cp_transport,
	.marker = cp_marker,
	.now_ms = cp_now,
	.sleep_ms = cp_sleep,
};

static bool over_ethernet(const struct web_auth_peer *local)
{
	ARG_UNUSED(local);
	return true;
}

static const struct web_api_v1_coprocessor cp_hooks = {
	.request_over_ethernet = over_ethernet,
};

/* -- the stores, the slot and the updater's board ------------------------------------ */

static int64_t clock_ms(void)
{
	return fake_now;
}

static const struct web_api_v1_firmware fw_hooks = {.now_ms = clock_ms};
static const struct web_api_v1_system sys_hooks = {
	.now_ms = clock_ms,
	.upload_max_bytes = MAX_IMAGE,
};

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(fw_lfs);

static struct fs_mount_t fw_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &fw_lfs,
	.storage_dev = (void *)PARTITION_ID(fw_test_partition),
	.mnt_point = MNT,
};
static bool fw_mounted;

static const struct flash_area *slot_fa;

static int s_read(void *c, uint32_t offset, uint8_t *buf, size_t len)
{
	ARG_UNUSED(c);
	return flash_area_read(slot_fa, offset, buf, len);
}

static int s_write(void *c, uint32_t offset, const uint8_t *data, size_t len)
{
	ARG_UNUSED(c);
	return flash_area_write(slot_fa, offset, data, len);
}

static int s_erase(void *c, uint32_t offset, uint32_t len)
{
	ARG_UNUSED(c);
	return flash_area_erase(slot_fa, offset, len);
}

/* As the board: the running image unconfirmed or a swap requested. */
static bool s_locked(void *c)
{
	ARG_UNUSED(c);
	return !fake_system.running_confirmed || fake_system.swap_pending;
}

static struct sys_img_platform slot_platform = {
	.read = s_read,
	.write = s_write,
	.erase = s_erase,
	.slot_locked = s_locked,
	.slot_size = SLOT_SIZE,
	.sector_size = SECTOR,
	.trailer_bytes = TRAILER,
	.image = {
		.header_size = 0x400,
		.ram = {{0x20000000U, 0x200C0000U}, {0x70000000U, 0x70800000U}},
		.exec = {0x02000000U, 0x02400000U},
	},
};

static int u_staged(void *c, const char *upload_id, struct system_image *out, uint32_t *size)
{
	struct sys_img_image img;
	int rc = sys_img_image_info(upload_id, &img);

	ARG_UNUSED(c);
	if (rc != 0) {
		return rc;
	}
	out->version = (struct system_image_version){
		.major = img.major, .minor = img.minor, .revision = img.revision, .build = img.build};
	memcpy(out->hash, img.image_hash, sizeof(out->hash));
	*size = img.size_bytes;
	return 0;
}

static int u_in_use(void *c, const char *upload_id, bool in_use)
{
	ARG_UNUSED(c);
	return sys_img_set_in_use(upload_id, in_use);
}

static void u_consumed(void *c, const char *upload_id)
{
	ARG_UNUSED(c);
	(void)sys_img_delete(upload_id);
}

static struct system_updater_platform upd_platform;

/* The updater reads the board at init: after changing the fake, start it again. */
static void updater_restart(void)
{
	upd_platform = fake_system_platform;
	upd_platform.staged_image = u_staged;
	upd_platform.set_upload_in_use = u_in_use;
	upd_platform.upload_consumed = u_consumed;
	zassert_ok(system_updater_init(&upd_platform));
}

/* A restart of the device as src/services/system opens the modules: the slot store
 * first (its RAM marks, such as an install's hold, are gone), then the updater. */
static void device_restart(void)
{
	zassert_ok(sys_img_init(DIR, &slot_platform, fake_now));
	updater_restart();
}

/* -- requests ------------------------------------------------------------------------- */

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

static void setup_device(void)
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
	memcpy(cookie, set_cookie, semi - set_cookie);
	cookie[semi - set_cookie] = '\0';
	zassert_true(json_string("csrf_token", csrf, sizeof(csrf)));
}

static void authorised(enum web_api_method method, const char *path, const char *key)
{
	request(method, path);
	ctx.req.headers.cookie = cookie;
	ctx.req.headers.csrf_token = csrf;
	ctx.req.headers.idempotency_key = key;
}

static void get(const char *path)
{
	request(WEB_API_GET, path);
	ctx.req.headers.cookie = cookie;
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

/* @p target NULL: the member is left out. */
static void create(const char *target, const char *key, size_t size, const char *sha)
{
	char json[320];
	char member[48] = "";

	if (target != NULL) {
		snprintf(member, sizeof(member), ",\"target\":\"%s\"", target);
	}
	snprintf(json, sizeof(json),
		 "{\"filename\":\"image.bin\",\"size_bytes\":%zu,\"sha256\":\"%s\"%s}", size, sha,
		 member);
	authorised(WEB_API_POST, "/api/v1/firmware/uploads", key);
	request_body(json);
	dispatch(&web_api_v1_router);
}

static void create_for(const char *target, const uint8_t *data, size_t len, const char *key,
		       char *id, size_t cap)
{
	char sha[65];

	sha256_hex_of(data, len, sha);
	create(target, key, len, sha);
	expect(201, NULL);
	zassert_true(json_string("id", id, cap), "%s", body);
}

static void put_chunk(const char *id, size_t offset, const uint8_t *data, size_t len,
		      const char *key)
{
	char path[128];

	snprintf(path, sizeof(path), "/api/v1/firmware/uploads/%s/data?offset=%zu", id, offset);
	authorised(WEB_API_PUT, path, key);
	request_octets(data, len, "application/octet-stream");
	dispatch(&web_api_v1_router);
}

static void post_json(const char *path, const char *json, const char *key)
{
	authorised(WEB_API_POST, path, key);
	request_body(json);
	dispatch(&web_api_v1_router);
}

static void verify(const char *id, const char *key)
{
	char path[96];

	snprintf(path, sizeof(path), "/api/v1/firmware/uploads/%s/verify", id);
	post_json(path, "{}", key);
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

	for (int i = 0; i < 5000; i++) {
		zassert_ok(job_get(job_id, &job));
		if (job_state_is_terminal(job.state)) {
			return job;
		}
		k_msleep(2);
	}
	zassert_unreachable("job %s never finished", job_id);
	return job;
}

static void upload_all(const char *id, const uint8_t *data, size_t len)
{
	for (size_t off = 0; off < len; off += CHUNK) {
		char key[32];
		char job_id[JOB_ID_MAX_LEN + 1];
		const size_t n = MIN(CHUNK, len - off);
		struct job_snapshot job;

		snprintf(key, sizeof(key), "chunk-key-%08zu", off);
		put_chunk(id, off, &data[off], n, key);
		accepted_job(job_id);
		job = wait_job(job_id);
		zassert_equal(job.state, JOB_STATE_SUCCEEDED, "chunk at %zu: state %d error %s", off,
			      job.state, job.has_error ? job.error.code : "-");
	}
}

static void wait_upload_idle(const char *id)
{
	for (int i = 0; i < 5000; i++) {
		get_upload(id);
		if (body_has("\"active_job_id\":null")) {
			return;
		}
		k_msleep(2);
	}
	zassert_unreachable("upload %s kept its job", id);
}

/* A verified STM32 image; its id in @p id. */
static void ready_stm32(const uint8_t *data, size_t len, char *id)
{
	char job_id[JOB_ID_MAX_LEN + 1];

	create_for("stm32u585", data, len, "create-key-stm32", id, SYS_IMG_ID_LEN + 1);
	upload_all(id, data, len);
	verify(id, "verify-key-stm32");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	wait_upload_idle(id);
}

/* An ESP32 upload that has not failed: created, nothing sent. */
static void staged_esp32(char *id)
{
	static const uint8_t bytes[4096];

	create_for(NULL, bytes, sizeof(bytes), "create-key-esp32", id, FW_STORE_ID_LEN + 1);
}

static void post_system_update(const char *upload_id, bool acknowledge, const char *key)
{
	char json[160];

	snprintf(json, sizeof(json), "{\"upload_id\":\"%s\",\"acknowledge_downgrade\":%s}",
		 upload_id, acknowledge ? "true" : "false");
	post_json("/api/v1/system/updates", json, key);
}

static void wait_reboot(void)
{
	for (int i = 0; i < 5000; i++) {
		if (fake_system.count[FSP_REBOOT] > 0U) {
			return;
		}
		k_msleep(2);
	}
	zassert_unreachable("the install never asked for the restart");
}

static void volume_fresh(void)
{
	const struct flash_area *fa;

	if (fw_mounted) {
		zassert_ok(fs_unmount(&fw_mnt));
		fw_mounted = false;
	}
	zassert_ok(flash_area_open(PARTITION_ID(fw_test_partition), &fa));
	zassert_ok(flash_area_erase(fa, 0, fa->fa_size));
	flash_area_close(fa);
	zassert_ok(fs_mount(&fw_mnt));
	fw_mounted = true;
}

/* -- suite ------------------------------------------------------------------------------ */

static void *suite_setup(void)
{
	zassert_ok(web_api_v1_init(&identity));
	zassert_ok(flash_area_open(PARTITION_ID(sysimg_slot_partition), &slot_fa));
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	harness_reset();
	zassert_ok(matter_service_init(&mf_platform));
	fake_storage_init(&net_storage);
	fake_storage_bind(&net_storage, &net_backend);
	zassert_ok(device_config_init(&net_backend, NULL));
	fake_net_init(&net);
	fake_net_bind(&net, &net_ops);
	network_manager_set_clock(net_clock);
	zassert_ok(network_manager_init(&net_ops));
	web_api_v1_set_network(&net_hooks);
	zassert_ok(log_store_init());
	zassert_ok(coprocessor_manager_init(&cp_platform));
	web_api_v1_set_coprocessor(&cp_hooks);

	volume_fresh();
	zassert_ok(fw_store_init(DIR, fake_now));
	web_api_v1_set_firmware(&fw_hooks);
	zassert_ok(flash_area_erase(slot_fa, 0, SLOT_SIZE));
	fake_system_init();
	zassert_ok(sys_img_init(DIR, &slot_platform, fake_now));
	updater_restart();
	web_api_v1_set_system(&sys_hooks);
	cookie[0] = '\0';
	csrf[0] = '\0';
	setup_device();
}

ZTEST_SUITE(v1_system, NULL, suite_setup, before, NULL, NULL);

/* -- uploads by target -------------------------------------------------------------------- */

ZTEST(v1_system, test_create_an_stm32_upload)
{
	char id[SYS_IMG_ID_LEN + 1];
	char again[SYS_IMG_ID_LEN + 1];
	char sha[65];
	char location[96];

	create_for("stm32u585", img_valid, sizeof(img_valid), "create-key-0001x", id, sizeof(id));
	zassert_mem_equal(id, "sysimg_", 7, "%s", id);
	zassert_true(body_has("\"target\":\"stm32u585\""), "%s", body);
	zassert_true(body_has("\"state\":\"receiving\""), "%s", body);
	snprintf(location, sizeof(location), "/api/v1/firmware/uploads/%s", id);
	zassert_str_equal(response_header("Location"), location);

	get_upload(id);
	expect(200, NULL);
	zassert_true(body_has("\"target\":\"stm32u585\""), "%s", body);
	zassert_true(body_has("\"image\":null"), "%s", body);

	/* A replay of the request is the same upload. */
	sha256_hex_of(img_valid, sizeof(img_valid), sha);
	create("stm32u585", "create-key-0001x", sizeof(img_valid), sha);
	expect(201, NULL);
	zassert_true(json_string("id", again, sizeof(again)));
	zassert_str_equal(again, id);
}

ZTEST(v1_system, test_without_target_the_upload_is_the_coprocessors)
{
	char id[FW_STORE_ID_LEN + 1];

	staged_esp32(id);
	zassert_mem_equal(id, "upload_", 7, "%s", id);
	zassert_true(body_has("\"target\":\"esp32c6\""), "%s", body);

	create("esp32c8", "create-key-0002x", 16, VALID_HASH);
	expect(422, "validation_failed");
}

ZTEST(v1_system, test_one_upload_across_both_targets)
{
	char esp[FW_STORE_ID_LEN + 1];
	char stm[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char sha[65];

	/* An ESP32 upload blocks an STM32 one, and the refusal names it. */
	staged_esp32(esp);
	sha256_hex_of(img_valid, sizeof(img_valid), sha);
	create("stm32u585", "create-key-0003a", sizeof(img_valid), sha);
	expect(409, "busy");
	zassert_true(body_has(esp), "the blocking upload is named: %s", body);
	zassert_true(body_has("of the ESP32 is staged"), "%s", body);
	delete_upload(esp, "delete-key-0003a");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);

	/* And the other way round. */
	create_for("stm32u585", img_valid, sizeof(img_valid), "create-key-0003b", stm, sizeof(stm));
	create(NULL, "create-key-0003c", 4096, sha);
	expect(409, "busy");
	zassert_true(body_has(stm), "the blocking upload is named: %s", body);
	zassert_true(body_has("of the STM32 is staged"), "%s", body);

	/* A failed upload blocks nothing. */
	delete_upload(stm, "delete-key-0003b");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	create_for("stm32u585", img_bad_magic, sizeof(img_bad_magic), "create-key-0003d", stm,
		   sizeof(stm));
	upload_all(stm, img_bad_magic, sizeof(img_bad_magic));
	verify(stm, "verify-key-0003d");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_FAILED);
	wait_upload_idle(stm);
	create(NULL, "create-key-0003e", 4096, sha);
	expect(201, NULL);
}

ZTEST(v1_system, test_stm32_create_refusals_in_order)
{
	char esp[FW_STORE_ID_LEN + 1];
	char sha[65];

	memset(sha, 'a', 64);
	sha[64] = '\0';

	/* busy, before invalid_state and 413 */
	staged_esp32(esp);
	fake_system.running_confirmed = false;
	updater_restart();
	create("stm32u585", "create-key-0004a", MAX_IMAGE + 1U, sha);
	expect(409, "busy");

	/* invalid_state (unconfirmed), before 413 */
	delete_upload(esp, "delete-key-0004a");
	expect(202, NULL);
	k_msleep(50);
	create("stm32u585", "create-key-0004b", MAX_IMAGE + 1U, sha);
	expect(409, "invalid_state");

	/* invalid_state (a swap is requested) */
	fake_system.running_confirmed = true;
	fake_system.swap_pending = true;
	updater_restart();
	create("stm32u585", "create-key-0004c", 4064, sha);
	expect(409, "invalid_state");

	/* 413 */
	fake_system.swap_pending = false;
	updater_restart();
	create("stm32u585", "create-key-0004d", MAX_IMAGE + 1U, sha);
	expect(413, "payload_too_large");
	create("stm32u585", "create-key-0004e", MAX_IMAGE, sha);
	expect(201, NULL);
}

ZTEST(v1_system, test_chunks_verify_and_delete_go_to_the_slot)
{
	char id[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char path[96];
	struct job_snapshot job;
	uint8_t head[16];

	create_for("stm32u585", img_valid_large, sizeof(img_valid_large), "create-key-0005x", id,
		   sizeof(id));
	put_chunk(id, CHUNK, img_valid_large, CHUNK, "chunk-key-0005aa");
	expect(409, "offset_mismatch");
	zassert_true(body_has("offset is 0"), "%s", body);

	upload_all(id, img_valid_large, sizeof(img_valid_large));
	wait_upload_idle(id);
	/* The bytes are in slot 2, not in a file. */
	zassert_ok(flash_area_read(slot_fa, 0, head, sizeof(head)));
	zassert_mem_equal(head, img_valid_large, sizeof(head));
	get_upload(id);
	snprintf(path, sizeof(path), "\"received_bytes\":%zu", sizeof(img_valid_large));
	zassert_true(body_has(path), "%s", body);

	verify(id, "verify-key-0005x");
	accepted_job(job_id);
	job = wait_job(job_id);
	zassert_equal(job.state, JOB_STATE_SUCCEEDED, "%s", job.has_error ? job.error.code : "-");
	snprintf(path, sizeof(path), "/api/v1/jobs/%s", job_id);
	get(path);
	snprintf(path, sizeof(path), "\"resource_url\":\"/api/v1/firmware/uploads/%s\"", id);
	zassert_true(body_has(path), "%s", body);

	wait_upload_idle(id);
	zassert_true(body_has("\"state\":\"ready\""), "%s", body);
	zassert_true(body_has("\"image\":{\"format\":\"mcuboot_image\",\"format_version\":null,"
			      "\"target\":\"stm32u585\",\"version\":\"2.0.1+77\",\"kind\":\"app\","
			      "\"partition_layout_id\":null,\"host_protocol\":null,"
			      "\"signature_verified\":null,\"allowed_methods\":[\"ota\"]}"),
		     "%s", body);

	delete_upload(id, "delete-key-0005x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	get_upload(id);
	expect(404, "not_found");
}

ZTEST(v1_system, test_a_chunk_while_the_slot_is_locked)
{
	char id[SYS_IMG_ID_LEN + 1];

	create_for("stm32u585", img_valid, sizeof(img_valid), "create-key-0006x", id, sizeof(id));
	fake_system.running_confirmed = false;
	put_chunk(id, 0, img_valid, sizeof(img_valid), "chunk-key-0006xx");
	expect(409, "invalid_state");
	get_upload(id);
	zassert_true(body_has("\"received_bytes\":0"), "%s", body);
}

ZTEST(v1_system, test_images_the_check_refuses)
{
	char id[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	struct job_snapshot job;

	create_for("stm32u585", img_bad_magic, sizeof(img_bad_magic), "create-key-0007a", id,
		   sizeof(id));
	upload_all(id, img_bad_magic, sizeof(img_bad_magic));
	verify(id, "verify-key-0007a");
	accepted_job(job_id);
	job = wait_job(job_id);
	zassert_equal(job.state, JOB_STATE_FAILED);
	zassert_str_equal(job.error.code, "invalid_image");
	wait_upload_idle(id);
	zassert_true(body_has("\"state\":\"failed\""), "%s", body);
	zassert_true(body_has("\"error\":{\"code\":\"invalid_image\""), "%s", body);
	zassert_true(body_has("\"image\":null"), "%s", body);

	/* A failed upload is replaced by the next create. */
	create_for("stm32u585", img_wrong_sp, sizeof(img_wrong_sp), "create-key-0007b", id,
		   sizeof(id));
	upload_all(id, img_wrong_sp, sizeof(img_wrong_sp));
	verify(id, "verify-key-0007b");
	accepted_job(job_id);
	job = wait_job(job_id);
	zassert_equal(job.state, JOB_STATE_FAILED);
	zassert_str_equal(job.error.code, "unsupported_target");
}

/* -- startSystemUpdate --------------------------------------------------------------------- */

ZTEST(v1_system, test_system_update_refusals_in_order)
{
	char esp[FW_STORE_ID_LEN + 1];
	char id[SYS_IMG_ID_LEN + 1];

	post_system_update("sysimg_0000000000000000", false, "update-key-0008a");
	expect(404, "not_found");
	post_system_update("upload_0123456789abcdef", false, "update-key-0008b");
	expect(422, "unsupported_target");
	staged_esp32(esp);
	post_system_update(esp, false, "update-key-0008c");
	expect(422, "unsupported_target");

	/* Not ready yet. */
	delete_upload(esp, "delete-key-0008c");
	expect(202, NULL);
	k_msleep(50);
	create_for("stm32u585", img_valid, sizeof(img_valid), "create-key-0008d", id, sizeof(id));
	post_system_update(id, false, "update-key-0008d");
	expect(409, "invalid_state");
	zassert_true(body_has("not verified"), "%s", body);

	/* Unconfirmed running firmware, before not-ready. */
	fake_system.running_confirmed = false;
	updater_restart();
	post_system_update(id, false, "update-key-0008e");
	expect(409, "invalid_state");
	zassert_true(body_has("not confirmed"), "%s", body);
}

ZTEST(v1_system, test_downgrade_needs_acknowledgement)
{
	char id[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];

	ready_stm32(img_valid, sizeof(img_valid), id);
	fake_system.running = fake_system_image(2, 0, 0, 0, 0xA0);
	updater_restart();

	post_system_update(id, false, "update-key-0009a");
	expect(422, "validation_failed");
	zassert_true(body_has("acknowledge_downgrade"), "%s", body);
	/* Nothing was left under the key: the same request is refused again, no job. */
	post_system_update(id, false, "update-key-0009a");
	expect(422, "validation_failed");
	get("/api/v1/system/status");
	zassert_true(body_has("\"active_job_ids\":[]"), "%s", body);

	post_system_update(id, true, "update-key-0009b");
	accepted_job(job_id);
	wait_reboot();
}

ZTEST(v1_system, test_the_same_version_is_a_reinstall)
{
	char id[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];

	ready_stm32(img_valid, sizeof(img_valid), id);
	fake_system.running = fake_system_image(1, 2, 3, 4, 0xA0);
	updater_restart();
	post_system_update(id, false, "update-key-0010x");
	accepted_job(job_id);
	wait_reboot();
}

ZTEST(v1_system, test_an_accepted_install_runs_to_the_restart)
{
	char id[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char again[JOB_ID_MAX_LEN + 1];
	char path[96];
	struct job_snapshot job;

	ready_stm32(img_valid, sizeof(img_valid), id);
	post_system_update(id, false, "update-key-0011x");
	accepted_job(job_id);
	zassert_true(body_has("\"resource_url\":\"/api/v1/system/firmware\""), "%s", body);
	snprintf(path, sizeof(path), "\"job_url\":\"/api/v1/jobs/%s\"", job_id);
	zassert_true(body_has(path), "%s", body);

	wait_reboot();
	zassert_equal(fake_system.count[FSP_REQUEST], 1U);
	zassert_true(fake_system.swap_pending);
	zassert_equal(fake_system.journal.stage, SYSTEM_UPDATE_STAGE_REBOOTING);
	zassert_str_equal(fake_system.in_use_id, "");

	zassert_ok(job_get(job_id, &job));
	zassert_equal(job.kind, JOB_KIND_SYSTEM_UPDATE);
	zassert_str_equal(job.phase, "rebooting");
	zassert_false(job_state_is_terminal(job.state), "the job ends with the device");
	snprintf(path, sizeof(path), "/api/v1/jobs/%s", job_id);
	get(path);
	expect(200, NULL);
	zassert_true(body_has("\"kind\":\"system_update\""), "%s", body);
	zassert_true(body_has("\"resource_url\":\"/api/v1/system/firmware\""), "%s", body);

	/* The upload is held by the install. */
	get_upload(id);
	delete_upload(id, "delete-key-0011x");
	expect(409, "busy");

	/* A replay is the install's job; another request is busy. */
	post_system_update(id, false, "update-key-0011x");
	expect(202, NULL);
	zassert_true(json_string("job_id", again, sizeof(again)));
	zassert_str_equal(again, job_id);
	post_system_update(id, false, "update-key-0011y");
	expect(409, "busy");
}

/* A restart in the middle of a network apply would roll it back (410 boot_changed). */
#define NET_IPV4_DHCP "{\"mode\":\"dhcp\",\"address\":null,\"prefix_length\":null,\"gateway\":null}"
#define NET_CANDIDATE                                                                              \
	"{\"base_revision\":0,\"config\":{\"preferred_interface\":\"ethernet\","                    \
	"\"dns\":{\"mode\":\"automatic\",\"servers\":[]},\"interfaces\":{"                          \
	"\"ethernet\":{\"enabled\":true,\"ipv4\":{\"mode\":\"static\",\"address\":\"192.168.88.50\"," \
	"\"prefix_length\":24,\"gateway\":\"192.168.88.1\"}},"                                      \
	"\"wifi\":{\"enabled\":false,\"ssid_base64\":\"\",\"security\":\"open\",\"hidden\":false,"  \
	"\"ipv4\":" NET_IPV4_DHCP ",\"credential\":{\"action\":\"keep\"}}}}}"

ZTEST(v1_system, test_a_network_change_being_applied_blocks_the_install)
{
	char id[SYS_IMG_ID_LEN + 1];
	char txn[64];
	char path[160];

	ready_stm32(img_valid, sizeof(img_valid), id);
	post_json("/api/v1/network/transactions", NET_CANDIDATE, "stage-key-0017xx");
	expect(201, NULL);
	zassert_true(json_string("id", txn, sizeof(txn)), "%s", body);
	snprintf(path, sizeof(path), "/api/v1/network/transactions/%s/apply", txn);
	post_json(path, "{\"confirmation_timeout_seconds\":120}", "apply-key-0017xx");
	expect(202, NULL);

	post_system_update(id, false, "update-key-0017x");
	expect(409, "busy");
	zassert_equal(fake_system.count[FSP_REQUEST], 0U);
}

/* The worker held, so the install's work stays queued. */
static K_SEM_DEFINE(gate, 0, 1);

static void gate_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	k_sem_take(&gate, K_FOREVER);
}

static K_WORK_DEFINE(gate_work, gate_fn);

ZTEST(v1_system, test_an_install_is_cancellable_until_it_runs)
{
	char id[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char path[96];
	struct job_snapshot job;

	ready_stm32(img_valid, sizeof(img_valid), id);
	k_sem_reset(&gate);
	zassert_true(v1_worker_submit(&gate_work) >= 0);
	k_msleep(20);

	post_system_update(id, false, "update-key-0018x");
	accepted_job(job_id);
	snprintf(path, sizeof(path), "/api/v1/jobs/%s", job_id);
	get(path);
	zassert_true(body_has("\"cancellable\":true"), "%s", body);
	snprintf(path, sizeof(path), "/api/v1/jobs/%s/cancel", job_id);
	post_json(path, "{}", "cancel-key-0018x");
	expect(202, NULL);

	k_sem_give(&gate);
	job = wait_job(job_id);
	zassert_equal(job.state, JOB_STATE_CANCELLED);
	zassert_equal(fake_system.count[FSP_REQUEST], 0U, "nothing reached MCUboot");
	zassert_equal(fake_system.count[FSP_REBOOT], 0U);
}

ZTEST(v1_system, test_coprocessor_install_refuses_an_stm32_upload)
{
	char id[SYS_IMG_ID_LEN + 1];
	char json[160];

	create_for("stm32u585", img_valid, sizeof(img_valid), "create-key-0012x", id, sizeof(id));
	snprintf(json, sizeof(json),
		 "{\"upload_id\":\"%s\",\"method\":\"uart\",\"acknowledge_recovery\":true}", id);
	post_json("/api/v1/coprocessor/updates", json, "update-key-0012x");
	expect(422, "unsupported_target");
}

/* -- getSystemFirmware and capabilities ------------------------------------------------------ */

ZTEST(v1_system, test_system_firmware_of_a_confirmed_image)
{
	get("/api/v1/system/firmware");
	expect(200, NULL);
	zassert_str_equal(body,
			  "{\"running\":{\"version\":\"1.0.0+0\",\"image_hash\":\""
			  "a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0\","
			  "\"confirmed\":true},\"confirm_remaining_seconds\":null,"
			  "\"swap_pending\":false,\"update\":{\"available\":true,\"reason\":null},"
			  "\"last_update\":null}");

	fake_system.running_confirmed = false;
	updater_restart();
	get("/api/v1/system/firmware");
	expect(200, NULL);
	zassert_true(body_has("\"confirmed\":false},\"confirm_remaining_seconds\":1200,"), "%s",
		     body);
	zassert_true(body_has("\"update\":{\"available\":false,\"reason\":\"firmware_unconfirmed\"}"),
		     "%s", body);
}

ZTEST(v1_system, test_system_firmware_through_an_install_and_the_restarts)
{
	char id[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char fragment[256];
	struct sys_img_image staged;
	struct system_image old_image = fake_system.running;
	struct system_image new_image;

	ready_stm32(img_valid, sizeof(img_valid), id);
	zassert_ok(sys_img_image_info(id, &staged));
	new_image = fake_system_image(staged.major, staged.minor, staged.revision, staged.build, 0);
	memcpy(new_image.hash, staged.image_hash, sizeof(new_image.hash));

	post_system_update(id, false, "update-key-0014x");
	accepted_job(job_id);
	wait_reboot();
	get("/api/v1/system/firmware");
	expect(200, NULL);
	zassert_true(body_has("\"update\":{\"available\":false,\"reason\":\"update_running\"}"), "%s",
		     body);
	zassert_true(body_has("\"swap_pending\":true"), "%s", body);
	get("/api/v1/capabilities");
	zassert_true(body_has("\"stm32_update\":{\"available\":false,\"reason\":\"update_running\"}"),
		     "%s", body);

	/* MCUboot swapped, the new image runs unconfirmed: the upload is gone. */
	fake_system_reboot_into(&new_image, false);
	device_restart();
	get("/api/v1/system/firmware");
	expect(200, NULL);
	zassert_true(body_has("\"running\":{\"version\":\"" VALID_VERSION "\",\"image_hash\":\""
			      VALID_HASH "\",\"confirmed\":false}"),
		     "%s", body);
	snprintf(fragment, sizeof(fragment),
		 "\"last_update\":{\"job_id\":\"%s\",\"state\":\"awaiting_confirmation\","
		 "\"from_version\":\"1.0.0+0\",\"version\":\"" VALID_VERSION "\",\"error\":null}",
		 job_id);
	zassert_true(body_has(fragment), "%s", body);
	get_upload(id);
	expect(404, "not_found");

	/* A reset before confirmation: MCUboot restored the old image. */
	fake_system_reboot_into(&old_image, true);
	device_restart();
	get("/api/v1/system/firmware");
	expect(200, NULL);
	zassert_true(body_has("\"state\":\"rolled_back\""), "%s", body);
	zassert_true(body_has("\"error\":{\"code\":\""), "an error object: %s", body);
	zassert_true(body_has("\"retryable\":"), "%s", body);
	zassert_true(body_has("\"update\":{\"available\":true,\"reason\":null}"), "%s", body);
}

ZTEST(v1_system, test_capabilities_of_the_stm32_update)
{
	get("/api/v1/capabilities");
	expect(200, NULL);
	zassert_true(body_has("\"stm32_update\":{\"available\":true,\"reason\":null}"), "%s", body);
	zassert_true(body_has("\"system_upload_max_bytes\":4128768,"), "%s", body);
	zassert_true(body_has("\"firmware_formats\":[\"raw_full_flash\",\"mcuboot_image\"]"), "%s",
		     body);

	fake_system.running_confirmed = false;
	updater_restart();
	get("/api/v1/capabilities");
	zassert_true(body_has("\"stm32_update\":{\"available\":false,"
			      "\"reason\":\"firmware_unconfirmed\"}"),
		     "%s", body);

	web_api_v1_set_system(NULL);
	get("/api/v1/capabilities");
	zassert_true(body_has("\"stm32_update\":{\"available\":false,"
			      "\"reason\":\"service_not_ready\"}"),
		     "%s", body);
}

ZTEST(v1_system, test_stm32_operations_answer_503_until_the_board_opened_them)
{
	char sha[65];

	web_api_v1_set_system(NULL);
	memset(sha, 'a', 64);
	sha[64] = '\0';
	create("stm32u585", "create-key-0016a", 4064, sha);
	expect(503, "service_not_ready");
	get_upload("sysimg_0000000000000000");
	expect(503, "service_not_ready");
	get("/api/v1/system/firmware");
	expect(503, "service_not_ready");
	post_system_update("sysimg_0000000000000000", false, "update-key-0016a");
	expect(503, "service_not_ready");
	/* The coprocessor's side does not depend on it. */
	create(NULL, "create-key-0016b", 4096, sha);
	expect(201, NULL);
}

ZTEST(v1_system, test_a_failed_coprocessor_upload_does_not_block_the_stm32)
{
	static const uint8_t zeros[4096];
	char esp[FW_STORE_ID_LEN + 1];
	char stm[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];

	staged_esp32(esp);
	upload_all(esp, zeros, sizeof(zeros));
	verify(esp, "verify-key-0019x");
	accepted_job(job_id);
	zassert_equal(wait_job(job_id).state, JOB_STATE_FAILED);
	wait_upload_idle(esp);
	create_for("stm32u585", img_valid, sizeof(img_valid), "create-key-0019x", stm, sizeof(stm));
}

ZTEST(v1_system, test_a_pending_chunk_is_the_uploads_active_job)
{
	char id[SYS_IMG_ID_LEN + 1];
	char job_id[JOB_ID_MAX_LEN + 1];
	char fragment[64];

	create_for("stm32u585", img_valid, sizeof(img_valid), "create-key-0020x", id, sizeof(id));
	k_sem_reset(&gate);
	zassert_true(v1_worker_submit(&gate_work) >= 0);
	k_msleep(20);
	put_chunk(id, 0, img_valid, sizeof(img_valid), "chunk-key-0020xx");
	accepted_job(job_id);
	get_upload(id);
	snprintf(fragment, sizeof(fragment), "\"active_job_id\":\"%s\"", job_id);
	zassert_true(body_has(fragment), "%s", body);
	k_sem_give(&gate);
	zassert_equal(wait_job(job_id).state, JOB_STATE_SUCCEEDED);
	wait_upload_idle(id);
}

/* The board's slot, not a constant of the bindings, bounds an STM32 upload. */
static const struct web_api_v1_system small_slot_hooks = {
	.now_ms = clock_ms,
	.upload_max_bytes = 1000000U,
};

ZTEST(v1_system, test_the_boards_limit_is_the_upload_limit)
{
	char sha[65];

	web_api_v1_set_system(&small_slot_hooks);
	get("/api/v1/capabilities");
	zassert_true(body_has("\"system_upload_max_bytes\":1000000,"), "%s", body);
	memset(sha, 'a', 64);
	sha[64] = '\0';
	create("stm32u585", "create-key-0021a", 1000001U, sha);
	expect(413, "payload_too_large");
	create("stm32u585", "create-key-0021b", 1000000U, sha);
	expect(201, NULL);
}
