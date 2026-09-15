/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * firmware-store tests: the corpus as byte arrays, the test volume, SHA-256.
 * The suites are in md5.c, image.c and store.c.
 */

#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include "common.h"

static const uint8_t fx_valid[] = {
#include "fw_valid.inc"
};
static const uint8_t fx_valid_16_segments[] = {
#include "fw_valid_16_segments.inc"
};
static const uint8_t fx_truncated[] = {
#include "fw_truncated.inc"
};
static const uint8_t fx_bootloader_wrong_chip[] = {
#include "fw_bootloader_wrong_chip.inc"
};
static const uint8_t fx_app_wrong_chip[] = {
#include "fw_app_wrong_chip.inc"
};
static const uint8_t fx_bare_app[] = {
#include "fw_bare_app.inc"
};
static const uint8_t fx_other_layout[] = {
#include "fw_other_layout.inc"
};
static const uint8_t fx_bad_table_md5[] = {
#include "fw_bad_table_md5.inc"
};
static const uint8_t fx_bad_app_sha[] = {
#include "fw_bad_app_sha.inc"
};
static const uint8_t fx_junk_in_nvs[] = {
#include "fw_junk_in_nvs.inc"
};
static const uint8_t fx_no_app_desc[] = {
#include "fw_no_app_desc.inc"
};
static const uint8_t fx_no_bootloader_desc[] = {
#include "fw_no_bootloader_desc.inc"
};

#define FX(n, c, m) {#n, fx_##n, sizeof(fx_##n), c, m}

/* The same outcomes as tests/fixtures/esp32/firmware/manifest.json. */
const struct fixture corpus[] = {
	FX(valid, NULL, NULL),
	FX(valid_16_segments, NULL, NULL),
	FX(truncated, "invalid_image", "application segment runs past the end of the file"),
	FX(bootloader_wrong_chip, "unsupported_target", "The bootloader is built for another chip"),
	FX(app_wrong_chip, "unsupported_target", "The application is built for another chip"),
	FX(bare_app, "invalid_image", "This is an application image"),
	FX(other_layout, "incompatible_firmware", "layout is not one this device supports"),
	FX(bad_table_md5, "invalid_image", "MD5 record does not match"),
	FX(bad_app_sha, "invalid_image", "The application's SHA-256 does not match"),
	FX(junk_in_nvs, "invalid_image", "NVS, otadata and phy_init"),
	FX(no_app_desc, "invalid_image", "has no application description"),
	FX(no_bootloader_desc, "invalid_image", "has no bootloader description"),
};
const size_t corpus_count = ARRAY_SIZE(corpus);

const struct fixture *fixture(const char *name)
{
	for (size_t i = 0; i < corpus_count; i++) {
		if (strcmp(corpus[i].name, name) == 0) {
			return &corpus[i];
		}
	}
	zassert_unreachable("no fixture %s", name);
	return NULL;
}

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(test_lfs);

static struct fs_mount_t mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &test_lfs,
	.storage_dev = (void *)PARTITION_ID(fw_test_partition),
	.mnt_point = TEST_MNT,
};
static bool mounted;

void volume_fresh(void)
{
	const struct flash_area *fa;

	if (mounted) {
		zassert_ok(fs_unmount(&mnt));
		mounted = false;
	}
	zassert_ok(flash_area_open(PARTITION_ID(fw_test_partition), &fa));
	zassert_ok(flash_area_erase(fa, 0, fa->fa_size));
	flash_area_close(fa);
	zassert_ok(fs_mount(&mnt));
	mounted = true;
}

void volume_remount(void)
{
	zassert_ok(fs_unmount(&mnt));
	zassert_ok(fs_mount(&mnt));
}

void sha256_of(const uint8_t *data, size_t len, uint8_t out[32])
{
	size_t n = 0;

	zassert_equal(psa_crypto_init(), PSA_SUCCESS);
	zassert_equal(psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &n), PSA_SUCCESS);
	zassert_equal(n, 32U);
}
