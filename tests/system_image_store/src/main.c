/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * system-image-store tests: the corpus as byte arrays, the slot platform over
 * the sim flash with fault injection, the metadata volume, SHA-256. The suites
 * are in image.c and store.c.
 */

#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include "common.h"

static const uint8_t img_valid[] = {
#include "img_valid.inc"
};
static const uint8_t img_valid_large[] = {
#include "img_valid_large.inc"
};
static const uint8_t img_valid_prot_tlv[] = {
#include "img_valid_prot_tlv.inc"
};
static const uint8_t img_bad_magic[] = {
#include "img_bad_magic.inc"
};
static const uint8_t img_header_0x200[] = {
#include "img_header_0x200.inc"
};
static const uint8_t img_flag_encrypted[] = {
#include "img_flag_encrypted.inc"
};
static const uint8_t img_flag_ram_load[] = {
#include "img_flag_ram_load.inc"
};
static const uint8_t img_truncated[] = {
#include "img_truncated.inc"
};
static const uint8_t img_trailing_bytes[] = {
#include "img_trailing_bytes.inc"
};
static const uint8_t img_bad_image_hash[] = {
#include "img_bad_image_hash.inc"
};
static const uint8_t img_no_sha_tlv[] = {
#include "img_no_sha_tlv.inc"
};
static const uint8_t img_bad_prot_tlv[] = {
#include "img_bad_prot_tlv.inc"
};
static const uint8_t img_wrong_sp[] = {
#include "img_wrong_sp.inc"
};
static const uint8_t img_wrong_reset[] = {
#include "img_wrong_reset.inc"
};
static const uint8_t img_reset_even[] = {
#include "img_reset_even.inc"
};
static const uint8_t img_esp32_merged[] = {
#include "img_esp32_merged.inc"
};

#define FX(n, c, m) {#n, img_##n, sizeof(img_##n), c, m}

/* The same outcomes as tests/fixtures/stm32/manifest.json. */
const struct fixture corpus[] = {
	FX(valid, NULL, NULL),
	FX(valid_large, NULL, NULL),
	FX(valid_prot_tlv, NULL, NULL),
	FX(bad_magic, "invalid_image", "header magic is missing"),
	FX(header_0x200, "unsupported_target", "header size is not the one"),
	FX(flag_encrypted, "invalid_image", "encrypted, compressed"),
	FX(flag_ram_load, "invalid_image", "encrypted, compressed"),
	FX(truncated, "invalid_image", "TLV area runs past the end"),
	FX(trailing_bytes, "invalid_image", "bytes after the image's TLV area"),
	FX(bad_image_hash, "invalid_image", "SHA-256 does not match the one in its TLV"),
	FX(no_sha_tlv, "invalid_image", "no SHA-256 TLV"),
	FX(bad_prot_tlv, "invalid_image", "protected TLV area is malformed"),
	FX(wrong_sp, "unsupported_target", "stack pointer is not in this board's RAM"),
	FX(wrong_reset, "unsupported_target", "reset vector is not in this board"),
	FX(reset_even, "unsupported_target", "reset vector is not in this board"),
};
const size_t corpus_count = ARRAY_SIZE(corpus);

const struct fixture esp32_merged = FX(esp32_merged, "invalid_image", "header magic is missing");

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

/* -- the slot --------------------------------------------------------------------- */

struct test_slot slot;

static const struct flash_area *slot_fa(void)
{
	static const struct flash_area *fa;

	if (fa == NULL) {
		zassert_ok(flash_area_open(PARTITION_ID(sysimg_slot_partition), &fa));
	}
	return fa;
}

static bool due(int *countdown)
{
	if (*countdown > 0) {
		(*countdown)--;
		return *countdown == 0;
	}
	return false;
}

static int t_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
{
	ARG_UNUSED(ctx);
	if (due(&slot.fail_read_in)) {
		return -EIO;
	}
	int rc = flash_area_read(slot_fa(), offset, buf, len);

	if (due(&slot.corrupt_read_in) && rc == 0 && len > 0U) {
		buf[len / 2U] ^= 0x10;
	}
	return rc;
}

static int t_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
	ARG_UNUSED(ctx);
	slot.write_calls++;
	if (due(&slot.fail_write_in)) {
		return -EIO;
	}
	if (due(&slot.write_fail_after_in)) {
		(void)flash_area_write(slot_fa(), offset, data, len);
		return -EIO;
	}
	return flash_area_write(slot_fa(), offset, data, len);
}

static int t_erase(void *ctx, uint32_t offset, uint32_t len)
{
	ARG_UNUSED(ctx);
	if (slot.erase_calls < (int)ARRAY_SIZE(slot.erase_off)) {
		slot.erase_off[slot.erase_calls] = offset;
		slot.erase_len[slot.erase_calls] = len;
	}
	slot.erase_calls++;
	if (due(&slot.fail_erase_in)) {
		return -EIO;
	}
	zassert_equal(offset % SECTOR, 0U, "erase at 0x%x not on a sector", offset);
	zassert_equal(len % SECTOR, 0U, "erase of 0x%x bytes not whole sectors", len);
	return flash_area_erase(slot_fa(), offset, len);
}

static bool t_locked(void *ctx)
{
	ARG_UNUSED(ctx);
	return slot.locked;
}

struct sys_img_platform platform = {
	.read = t_read,
	.write = t_write,
	.erase = t_erase,
	.slot_locked = t_locked,
	.slot_size = SLOT_SIZE,
	.sector_size = SECTOR,
	.trailer_bytes = TRAILER,
	.image = {
		.header_size = TEST_HDR,
		.ram = {{RAM_START, RAM_END}, {RAM2_START, RAM2_END}},
		.exec = {EXEC_START, EXEC_END},
	},
};

void slot_reset_knobs(void)
{
	memset(&slot, 0, sizeof(slot));
}

void slot_stream(bool on)
{
	platform.flash_dev = on ? flash_area_get_device(slot_fa()) : NULL;
	platform.flash_offset = on ? (uint32_t)slot_fa()->fa_off : 0U;
}

void slot_erase_all(void)
{
	zassert_ok(flash_area_erase(slot_fa(), 0, SLOT_SIZE));
}

void slot_raw_write(uint32_t offset, const uint8_t *data, size_t len)
{
	zassert_ok(flash_area_write(slot_fa(), offset, data, len));
}

void slot_raw_read(uint32_t offset, uint8_t *data, size_t len)
{
	zassert_ok(flash_area_read(slot_fa(), offset, data, len));
}

void slot_raw_fill(uint32_t offset, uint8_t value, size_t len)
{
	uint8_t buf[256];

	memset(buf, value, sizeof(buf));
	for (size_t done = 0; done < len; done += sizeof(buf)) {
		slot_raw_write(offset + done, buf, MIN(sizeof(buf), len - done));
	}
}

/* -- the volume ------------------------------------------------------------------- */

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(test_lfs);

static struct fs_mount_t mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &test_lfs,
	.storage_dev = (void *)PARTITION_ID(sysimg_lfs_partition),
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
	zassert_ok(flash_area_open(PARTITION_ID(sysimg_lfs_partition), &fa));
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
