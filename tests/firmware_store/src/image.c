/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fw_image: the full-flash check on the corpus, fed in pieces of every size, and
 * on single-byte faults placed where each rule looks.
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <firmware_store/fw_image.h>
#include <firmware_store/fw_md5.h>

#include "common.h"

static struct fw_image_check check;
static uint8_t work[0x11000];

static enum fw_image_result run(const uint8_t *data, size_t len, uint32_t declared, size_t piece,
				struct fw_image_info *info, const char **message)
{
	fw_image_check_begin(&check, declared);
	for (size_t off = 0; off < len; off += piece) {
		fw_image_check_feed(&check, &data[off], MIN(piece, len - off));
	}
	return fw_image_check_end(&check, info, message);
}

static enum fw_image_result run_whole(const uint8_t *data, size_t len, const char **message)
{
	struct fw_image_info info;

	return run(data, len, (uint32_t)len, len, &info, message);
}

static void assert_message(const char *message, const char *fragment)
{
	zassert_not_null(message);
	zassert_not_null(strstr(message, fragment), "\"%s\" lacks \"%s\"", message, fragment);
}

/* A copy of the valid file to damage. */
static const uint8_t *valid_copy(size_t *len)
{
	const struct fixture *f = fixture("valid");

	zassert_true(f->len <= sizeof(work));
	memcpy(work, f->data, f->len);
	*len = f->len;
	return work;
}

static void expect_fault(size_t len, const char *code, const char *fragment)
{
	const char *message = NULL;
	enum fw_image_result r = run_whole(work, len, &message);

	zassert_str_equal(fw_image_result_code(r) ? fw_image_result_code(r) : "ok", code);
	assert_message(message, fragment);
}

ZTEST(fw_image, test_corpus_outcomes)
{
	for (size_t i = 0; i < corpus_count; i++) {
		const struct fixture *f = &corpus[i];
		const char *message = NULL;
		enum fw_image_result r = run_whole(f->data, f->len, &message);

		if (f->code == NULL) {
			zassert_equal(r, FW_IMAGE_OK, "%s: %s", f->name, message ? message : "");
			zassert_is_null(message);
		} else {
			zassert_not_null(fw_image_result_code(r), "%s accepted", f->name);
			zassert_str_equal(fw_image_result_code(r), f->code, "%s", f->name);
			assert_message(message, f->message);
		}
	}
}

ZTEST(fw_image, test_outcome_does_not_depend_on_piece_size)
{
	static const size_t pieces[] = {1, 3, 31, 32, 4096, 16384, 0x8001};

	for (size_t i = 0; i < corpus_count; i++) {
		const struct fixture *f = &corpus[i];
		const char *whole_message = NULL;
		const enum fw_image_result whole = run_whole(f->data, f->len, &whole_message);

		for (size_t p = 0; p < ARRAY_SIZE(pieces); p++) {
			struct fw_image_info info;
			const char *message = NULL;
			enum fw_image_result r =
				run(f->data, f->len, (uint32_t)f->len, pieces[p], &info, &message);

			zassert_equal(r, whole, "%s in pieces of %zu", f->name, pieces[p]);
			zassert_equal(message, whole_message, "%s in pieces of %zu", f->name, pieces[p]);
		}
	}
}

ZTEST(fw_image, test_valid_file_describes_itself)
{
	const struct fixture *f = fixture("valid");
	struct fw_image_info info;
	const char *message = NULL;
	uint8_t digest[32];

	zassert_equal(run(f->data, f->len, (uint32_t)f->len, 4096, &info, &message), FW_IMAGE_OK);
	zassert_str_equal(info.version, "1.2.3-synthetic");
	zassert_str_equal(info.project_name, "eh_cp_c6_cedar");
	zassert_str_equal(info.idf_version, "v5.5.5-synthetic");
	zassert_str_equal(info.layout_id, "cedar-c6-ota-4m-2x1792k");
	zassert_str_equal(info.host_protocol, "esp-hosted-mcu-3");
	/* Header 24 + two segment headers + 180 + 77 data, padded to 16, + digest. */
	zassert_equal(info.bootloader_bytes, 336U);
	zassert_equal(info.app_bytes, (uint32_t)f->len - FW_IMAGE_APP_OFFSET);
	sha256_of(f->data, f->len, digest);
	zassert_mem_equal(info.sha256, digest, sizeof(digest));
}

ZTEST(fw_image, test_incompatible_layout_keeps_the_description)
{
	const struct fixture *f = fixture("other_layout");
	struct fw_image_info info;
	const char *message = NULL;

	zassert_equal(run(f->data, f->len, (uint32_t)f->len, f->len, &info, &message),
		      FW_IMAGE_INCOMPATIBLE);
	zassert_is_null(info.layout_id);
	zassert_is_null(info.host_protocol);
	zassert_str_equal(info.version, "1.2.3-synthetic");
}

ZTEST(fw_image, test_sizes_that_cannot_be_images)
{
	const struct fixture *f = fixture("valid");
	struct fw_image_info info;
	const char *message = NULL;

	/* Declared past ota_1: refused whatever the bytes are. */
	zassert_equal(run(f->data, f->len, FW_IMAGE_MAX_BYTES + 1U, f->len, &info, &message),
		      FW_IMAGE_INVALID);
	assert_message(message, "reaches ota_1");

	/* Exactly the region before the application, nothing after. */
	zassert_equal(run(f->data, FW_IMAGE_APP_OFFSET, FW_IMAGE_APP_OFFSET, 4096, &info,
			  &message),
		      FW_IMAGE_INVALID);
	assert_message(message, "ends before the application");

	/* Fed less than declared. */
	zassert_equal(run(f->data, f->len - 10U, (uint32_t)f->len, 4096, &info, &message),
		      FW_IMAGE_INVALID);
	assert_message(message, "shorter than its declared size");
	zassert_equal(info.app_bytes, 0U);
}

ZTEST(fw_image, test_bytes_past_the_declared_size_are_ignored)
{
	const struct fixture *f = fixture("valid");
	struct fw_image_info info;
	const char *message = NULL;

	fw_image_check_begin(&check, (uint32_t)f->len);
	fw_image_check_feed(&check, f->data, f->len);
	fw_image_check_feed(&check, f->data, 100);
	zassert_equal(fw_image_check_end(&check, &info, &message), FW_IMAGE_OK);
}

ZTEST(fw_image, test_trailing_byte_after_the_application)
{
	size_t len;

	valid_copy(&len);
	work[len] = 0xFF;
	expect_fault(len + 1U, "invalid_image", "Bytes follow the end of the application");
}

ZTEST(fw_image, test_bootloader_faults)
{
	size_t len;

	valid_copy(&len);
	work[0] = 0x00;
	expect_fault(len, "invalid_image", "No ESP image header at 0x0");

	valid_copy(&len);
	work[1] = 0;
	expect_fault(len, "invalid_image", "impossible segment count");

	valid_copy(&len);
	work[1] = 17;
	expect_fault(len, "invalid_image", "impossible segment count");

	valid_copy(&len);
	work[23] = 0;
	expect_fault(len, "invalid_image", "no appended SHA-256");

	/* A byte of segment data: the checksum sees it before the digest does. */
	valid_copy(&len);
	work[0x100] ^= 0x01;
	expect_fault(len, "invalid_image", "bootloader's checksum does not match");

	/* The checksum byte itself: 336 - 33. */
	valid_copy(&len);
	work[303] ^= 0x01;
	expect_fault(len, "invalid_image", "bootloader's checksum does not match");

	/* The digest. */
	valid_copy(&len);
	work[335] ^= 0x80;
	expect_fault(len, "invalid_image", "bootloader's SHA-256 does not match");

	/* The first byte after the bootloader. */
	valid_copy(&len);
	work[336] = 0x00;
	expect_fault(len, "invalid_image", "between the bootloader and the partition table");

	/* The last byte of the bootloader's region. */
	valid_copy(&len);
	work[FW_IMAGE_TABLE_OFFSET - 1U] = 0x7F;
	expect_fault(len, "invalid_image", "between the bootloader and the partition table");
}

ZTEST(fw_image, test_bootloader_segment_past_its_region)
{
	size_t len;

	/* The second segment's length reaching past 0x8000. */
	valid_copy(&len);
	sys_put_le32(0x8000, &work[24 + 8 + 180 + 4]);
	expect_fault(len, "invalid_image", "bootloader segment runs past 0x8000");

	/* The first segment's length reaching past 0x8000: runs into the table. */
	valid_copy(&len);
	sys_put_le32(0x9000, &work[24 + 4]);
	expect_fault(len, "invalid_image", "The bootloader runs past 0x8000");

	/* A length no image can have. */
	valid_copy(&len);
	sys_put_le32(0xFFFFFFFF, &work[24 + 4]);
	expect_fault(len, "invalid_image", "bootloader segment runs past 0x8000");

	/* The first segment too short for the description. */
	valid_copy(&len);
	sys_put_le32(79, &work[24 + 4]);
	expect_fault(len, "invalid_image", "too short to hold the image description");
}

ZTEST(fw_image, test_partition_table_faults)
{
	const size_t md5_record = FW_IMAGE_TABLE_OFFSET + 5U * 32U;
	size_t len;

	/* No table at all. */
	valid_copy(&len);
	memset(&work[FW_IMAGE_TABLE_OFFSET], 0xFF, md5_record + 32U - FW_IMAGE_TABLE_OFFSET);
	expect_fault(len, "invalid_image", "No partition table at 0x8000");

	/* Garbage where the table starts. */
	valid_copy(&len);
	work[FW_IMAGE_TABLE_OFFSET] = 0x00;
	expect_fault(len, "invalid_image", "No partition table at 0x8000");

	/* A corrupt third entry. */
	valid_copy(&len);
	sys_put_le16(0x1234, &work[FW_IMAGE_TABLE_OFFSET + 64U]);
	expect_fault(len, "invalid_image", "corrupt entry");

	/* The MD5 record erased: entries then the end, no digest. */
	valid_copy(&len);
	memset(&work[md5_record], 0xFF, 32);
	expect_fault(len, "invalid_image", "has no MD5 record");

	/* An entry after the MD5 record. */
	valid_copy(&len);
	memcpy(&work[md5_record + 32U], &work[FW_IMAGE_TABLE_OFFSET], 32);
	expect_fault(len, "invalid_image", "corrupt entry");

	/* A second MD5 record. */
	valid_copy(&len);
	memcpy(&work[md5_record + 32U], &work[md5_record], 32);
	expect_fault(len, "invalid_image", "corrupt entry");

	/* A byte after the end marker inside the table's 0xC00. */
	valid_copy(&len);
	work[FW_IMAGE_TABLE_OFFSET + FW_IMAGE_TABLE_MAX_LEN - 1U] = 0x00;
	expect_fault(len, "invalid_image", "after the partition table's end are not erased");

	/* A table that never ends. */
	valid_copy(&len);
	for (size_t i = 6; i * 32U < FW_IMAGE_TABLE_MAX_LEN; i++) {
		memcpy(&work[FW_IMAGE_TABLE_OFFSET + i * 32U], &work[FW_IMAGE_TABLE_OFFSET], 32);
	}
	expect_fault(len, "invalid_image", "corrupt entry");

	/* Between the table's region and NVS. */
	valid_copy(&len);
	work[FW_IMAGE_TABLE_OFFSET + FW_IMAGE_TABLE_MAX_LEN] = 0x00;
	expect_fault(len, "invalid_image", "Bytes after the partition table are not erased");

	/* The last byte of phy_init. */
	valid_copy(&len);
	work[FW_IMAGE_APP_OFFSET - 1U] = 0x00;
	expect_fault(len, "invalid_image", "NVS, otadata and phy_init");
}

/* Every field of every entry decides the layout; a changed one with a correct MD5. */
ZTEST(fw_image, test_every_layout_field_counts)
{
	static const size_t fields[] = {2, 3, 4, 8, 12, 13, 27, 28};

	for (size_t entry = 0; entry < 5; entry++) {
		for (size_t f = 0; f < ARRAY_SIZE(fields); f++) {
			const size_t at = FW_IMAGE_TABLE_OFFSET + entry * 32U + fields[f];
			const size_t md5_record = FW_IMAGE_TABLE_OFFSET + 5U * 32U;
			const char *message = NULL;
			size_t len;

			valid_copy(&len);
			work[at] ^= 0x01;
			fw_md5(&work[FW_IMAGE_TABLE_OFFSET], 5U * 32U, &work[md5_record + 16U]);
			zassert_equal(run_whole(work, len, &message), FW_IMAGE_INCOMPATIBLE,
				      "entry %zu byte %zu", entry, fields[f]);
		}
	}
}

ZTEST(fw_image, test_one_entry_less_or_more)
{
	const size_t md5_record = FW_IMAGE_TABLE_OFFSET + 5U * 32U;
	const char *message = NULL;
	size_t len;

	/* Four entries: move the MD5 record up and recompute it. */
	valid_copy(&len);
	memcpy(&work[md5_record - 32U], &work[md5_record], 32);
	memset(&work[md5_record], 0xFF, 32);
	fw_md5(&work[FW_IMAGE_TABLE_OFFSET], 4U * 32U, &work[md5_record - 32U + 16U]);
	zassert_equal(run_whole(work, len, &message), FW_IMAGE_INCOMPATIBLE);

	/* Six entries. */
	valid_copy(&len);
	memcpy(&work[md5_record], &work[md5_record - 32U], 32);
	memset(&work[md5_record + 32U], 0xFF, 32);
	work[md5_record + 32U] = 0xEB;
	work[md5_record + 33U] = 0xEB;
	fw_md5(&work[FW_IMAGE_TABLE_OFFSET], 6U * 32U, &work[md5_record + 32U + 16U]);
	zassert_equal(run_whole(work, len, &message), FW_IMAGE_INCOMPATIBLE);
}

ZTEST(fw_image, test_application_faults)
{
	const size_t app = FW_IMAGE_APP_OFFSET;
	size_t len;

	valid_copy(&len);
	work[app] = 0x00;
	expect_fault(len, "invalid_image", "No application image header at 0x10000");

	valid_copy(&len);
	work[app + 1U] = 0;
	expect_fault(len, "invalid_image", "impossible segment count");

	valid_copy(&len);
	work[app + 23U] = 2;
	expect_fault(len, "invalid_image", "no appended SHA-256");

	valid_copy(&len);
	sys_put_le32(0x00FFFFFF, &work[app + 24U + 4U]);
	expect_fault(len, "invalid_image", "application segment runs past the end of the file");

	/* A later segment past the end. */
	valid_copy(&len);
	sys_put_le32(0x10000, &work[app + 24U + 8U + 467U + 4U]);
	expect_fault(len, "invalid_image", "application segment runs past the end of the file");

	valid_copy(&len);
	sys_put_le32(175, &work[app + 24U + 4U]);
	expect_fault(len, "invalid_image", "too short to hold the image description");

	/* Not text in the version, and an empty version. */
	valid_copy(&len);
	work[app + 32U + 16U] = 0x01;
	expect_fault(len, "invalid_image", "does not hold text");

	valid_copy(&len);
	work[app + 32U + 16U] = 0x00;
	expect_fault(len, "invalid_image", "does not hold text");

	/* A version filling its 32 bytes with no terminator. */
	valid_copy(&len);
	memset(&work[app + 32U + 16U], 'v', 32);
	expect_fault(len, "invalid_image", "does not hold text");

	/* A byte of the last segment: checksum. */
	valid_copy(&len);
	work[len - 60U] ^= 0x01;
	expect_fault(len, "invalid_image", "application's checksum does not match");
}

ZTEST(fw_image, test_empty_idf_version_is_accepted)
{
	const char *message = NULL;
	size_t len;
	const size_t app = FW_IMAGE_APP_OFFSET;

	valid_copy(&len);
	/* Blank the IDF text, then fix the checksum and digest the change broke. */
	uint8_t old_xor = 0;

	for (size_t i = 0; i < FW_IMAGE_TEXT_MAX; i++) {
		old_xor ^= work[app + 32U + 112U + i];
		work[app + 32U + 112U + i] = 0;
	}
	const size_t checksum_at = len - 33U;

	work[checksum_at] ^= old_xor;
	sha256_of(&work[app], checksum_at + 1U - app, &work[checksum_at + 1U]);
	zassert_equal(run_whole(work, len, &message), FW_IMAGE_OK, "%s", message ? message : "");
}

ZTEST(fw_image, test_structural_fault_outranks_a_layout_mismatch)
{
	const struct fixture *f = fixture("other_layout");
	size_t len = f->len;

	memcpy(work, f->data, len);
	work[0xA000] = 0x00;
	expect_fault(len, "invalid_image", "NVS, otadata and phy_init");

	memcpy(work, f->data, len);
	sys_put_le16(5, &work[FW_IMAGE_APP_OFFSET + 12U]);
	expect_fault(len, "unsupported_target", "application is built for another chip");
}

ZTEST(fw_image, test_failed_is_reported_early)
{
	const struct fixture *f = fixture("bootloader_wrong_chip");

	fw_image_check_begin(&check, (uint32_t)f->len);
	zassert_false(fw_image_check_failed(&check));
	fw_image_check_feed(&check, f->data, 24);
	zassert_true(fw_image_check_failed(&check));
	fw_image_check_abort(&check);
}

ZTEST(fw_image, test_whole_file_digest_survives_a_fault)
{
	const struct fixture *f = fixture("junk_in_nvs");
	struct fw_image_info info;
	const char *message = NULL;
	uint8_t digest[32];

	zassert_equal(run(f->data, f->len, (uint32_t)f->len, 1000, &info, &message),
		      FW_IMAGE_INVALID);
	sha256_of(f->data, f->len, digest);
	zassert_mem_equal(info.sha256, digest, sizeof(digest));
}

ZTEST(fw_image, test_description_text_is_printable_ascii)
{
	size_t len;

	valid_copy(&len);
	work[FW_IMAGE_APP_OFFSET + 32U + 16U + 1U] = 0x7F;
	expect_fault(len, "invalid_image", "does not hold text");

	valid_copy(&len);
	work[FW_IMAGE_APP_OFFSET + 32U + 48U] = 0x1F;
	expect_fault(len, "invalid_image", "does not hold text");
}

ZTEST(fw_image, test_every_md5_byte_counts)
{
	const size_t md5_record = FW_IMAGE_TABLE_OFFSET + 5U * 32U;
	size_t len;

	valid_copy(&len);
	work[md5_record + 31U] ^= 0x01;
	expect_fault(len, "invalid_image", "MD5 record does not match");
}

ZTEST(fw_image, test_table_without_an_end)
{
	size_t len;

	/* 96 entries fill the table's 0xC00: no MD5, no end marker. */
	valid_copy(&len);
	for (size_t i = 1; i * 32U < FW_IMAGE_TABLE_MAX_LEN; i++) {
		memcpy(&work[FW_IMAGE_TABLE_OFFSET + i * 32U], &work[FW_IMAGE_TABLE_OFFSET], 32);
	}
	expect_fault(len, "invalid_image", "has no end");
}

ZTEST(fw_image, test_largest_declared_size_is_not_refused_for_size)
{
	const struct fixture *f = fixture("valid");
	struct fw_image_info info;
	const char *message = NULL;

	zassert_equal(run(f->data, f->len, FW_IMAGE_MAX_BYTES, f->len, &info, &message),
		      FW_IMAGE_INVALID);
	assert_message(message, "shorter than its declared size");
}

ZTEST(fw_image, test_application_cut_inside_its_digest)
{
	const struct fixture *f = fixture("valid");
	struct fw_image_info info;
	const char *message = NULL;

	zassert_equal(run(f->data, f->len - 10U, (uint32_t)f->len - 10U, 4096, &info, &message),
		      FW_IMAGE_INVALID);
	assert_message(message, "The application is cut short");
}

ZTEST(fw_image, test_no_layout_after_a_structural_fault)
{
	const struct fixture *f = fixture("junk_in_nvs");
	struct fw_image_info info;
	const char *message = NULL;

	zassert_equal(run(f->data, f->len, (uint32_t)f->len, f->len, &info, &message),
		      FW_IMAGE_INVALID);
	zassert_is_null(info.layout_id);
	zassert_is_null(info.host_protocol);
}

ZTEST(fw_image, test_first_application_segment_past_the_file)
{
	size_t len;

	/* Within the size an image may have, beyond this file: only the bootloader's
	 * first segment is allowed to overrun until its description is read. */
	valid_copy(&len);
	sys_put_le32(0x10000, &work[FW_IMAGE_APP_OFFSET + 24U + 4U]);
	expect_fault(len, "invalid_image", "application segment runs past the end of the file");
}

ZTEST(fw_image, test_md5_record_before_any_entry)
{
	static const uint8_t empty_md5[16] = {0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04,
					      0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e};
	size_t len;

	valid_copy(&len);
	memset(&work[FW_IMAGE_TABLE_OFFSET], 0xFF, 32);
	work[FW_IMAGE_TABLE_OFFSET] = 0xEB;
	work[FW_IMAGE_TABLE_OFFSET + 1U] = 0xEB;
	memcpy(&work[FW_IMAGE_TABLE_OFFSET + 16U], empty_md5, sizeof(empty_md5));
	expect_fault(len, "invalid_image", "No partition table at 0x8000");
}

ZTEST(fw_image, test_one_piece_running_past_the_declared_size)
{
	struct fw_image_info info;
	const char *message = NULL;
	size_t len;

	valid_copy(&len);
	memset(&work[len], 0x00, 100);
	fw_image_check_begin(&check, (uint32_t)len);
	fw_image_check_feed(&check, work, len + 100U);
	zassert_equal(fw_image_check_end(&check, &info, &message), FW_IMAGE_OK, "%s",
		      message ? message : "");
	sha256_of(work, len, work + len);
	zassert_mem_equal(info.sha256, work + len, 32);
}

ZTEST(fw_image, test_result_codes)
{
	zassert_is_null(fw_image_result_code(FW_IMAGE_OK));
	zassert_str_equal(fw_image_result_code(FW_IMAGE_INVALID), "invalid_image");
	zassert_str_equal(fw_image_result_code(FW_IMAGE_UNSUPPORTED_TARGET), "unsupported_target");
	zassert_str_equal(fw_image_result_code(FW_IMAGE_INCOMPATIBLE), "incompatible_firmware");
}

ZTEST_SUITE(fw_image, NULL, NULL, NULL, NULL, NULL);
