/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mcuboot_image: the corpus, fed whole and in pieces, and images built here for
 * the boundaries the corpus does not reach (every refused flag, the RAM and
 * execution ranges, the TLV area's limits).
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <system_image_store/mcuboot_image.h>

#include "common.h"

static struct mcuboot_image_check chk;
static struct mcuboot_image_info info;
static const char *why;
static uint8_t buf[16384];

static enum mcuboot_image_result run(const uint8_t *data, size_t len, size_t piece)
{
	mcuboot_image_check_begin(&chk, &platform.image, (uint32_t)len);
	for (size_t off = 0; off < len; off += piece) {
		mcuboot_image_check_feed(&chk, &data[off], MIN(piece, len - off));
	}
	return mcuboot_image_check_end(&chk, &info, &why);
}

static void expect(const struct fixture *fx, enum mcuboot_image_result r)
{
	if (fx->code == NULL) {
		zassert_equal(r, MCUBOOT_IMAGE_OK, "%s: %s", fx->name, why ? why : "");
		zassert_is_null(why, "%s", fx->name);
	} else {
		zassert_not_equal(r, MCUBOOT_IMAGE_OK, "%s accepted", fx->name);
		zassert_str_equal(mcuboot_image_result_code(r), fx->code, "%s: %s", fx->name, why);
		zassert_not_null(why);
		zassert_not_null(strstr(why, fx->message), "%s: '%s'", fx->name, why);
	}
}

ZTEST_SUITE(mcuboot_image, NULL, NULL, NULL, NULL, NULL);

ZTEST(mcuboot_image, test_corpus_whole)
{
	for (size_t i = 0; i < corpus_count; i++) {
		expect(&corpus[i], run(corpus[i].data, corpus[i].len, corpus[i].len));
	}
}

ZTEST(mcuboot_image, test_corpus_in_pieces)
{
	static const size_t pieces[] = {1, 7, 31, 32, 33, 1023, 4096};

	for (size_t p = 0; p < ARRAY_SIZE(pieces); p++) {
		for (size_t i = 0; i < corpus_count; i++) {
			expect(&corpus[i], run(corpus[i].data, corpus[i].len, pieces[p]));
		}
	}
}

ZTEST(mcuboot_image, test_valid_reports_version_hashes)
{
	const struct fixture *fx = fixture("valid");
	uint8_t digest[32];

	zassert_equal(run(fx->data, fx->len, 1000), MCUBOOT_IMAGE_OK);
	zassert_str_equal(info.version, "1.2.3+4");
	zassert_equal(info.major, 1);
	zassert_equal(info.minor, 2);
	zassert_equal(info.revision, 3);
	zassert_equal(info.build, 4);
	/* Unsigned imgtool output: the SHA-256 TLV is the last 32 bytes. */
	zassert_mem_equal(info.image_hash, &fx->data[fx->len - 32], 32);
	sha256_of(fx->data, fx->len, digest);
	zassert_mem_equal(info.sha256, digest, 32);

	fx = fixture("valid_large");
	zassert_equal(run(fx->data, fx->len, 4096), MCUBOOT_IMAGE_OK);
	zassert_str_equal(info.version, "2.0.1+77");

	fx = fixture("valid_prot_tlv");
	zassert_equal(run(fx->data, fx->len, 4096), MCUBOOT_IMAGE_OK);
	zassert_mem_equal(info.image_hash, &fx->data[fx->len - 32], 32);
}

ZTEST(mcuboot_image, test_esp32_file_is_not_an_image)
{
	expect(&esp32_merged, run(esp32_merged.data, esp32_merged.len, 4096));
}

ZTEST(mcuboot_image, test_bytes_past_the_size_are_ignored)
{
	const struct fixture *fx = fixture("valid");

	memcpy(buf, fx->data, fx->len);
	memset(&buf[fx->len], 0x55, 100);
	mcuboot_image_check_begin(&chk, &platform.image, (uint32_t)fx->len);
	mcuboot_image_check_feed(&chk, buf, fx->len + 100);
	mcuboot_image_check_feed(&chk, buf, 10);
	mcuboot_image_check_feed(&chk, NULL, 10);
	zassert_equal(mcuboot_image_check_end(&chk, &info, &why), MCUBOOT_IMAGE_OK, "%s", why);
}

ZTEST(mcuboot_image, test_null_feed_mid_file_is_ignored)
{
	const struct fixture *fx = fixture("valid");

	mcuboot_image_check_begin(&chk, &platform.image, (uint32_t)fx->len);
	mcuboot_image_check_feed(&chk, fx->data, 100);
	mcuboot_image_check_feed(&chk, NULL, 10);
	mcuboot_image_check_feed(&chk, &fx->data[100], 0);
	mcuboot_image_check_feed(&chk, &fx->data[100], fx->len - 100);
	zassert_equal(mcuboot_image_check_end(&chk, &info, &why), MCUBOOT_IMAGE_OK, "%s", why);
}

ZTEST(mcuboot_image, test_short_feed_fails_without_digest)
{
	const struct fixture *fx = fixture("valid");
	static const uint8_t zero[32];

	mcuboot_image_check_begin(&chk, &platform.image, (uint32_t)fx->len);
	mcuboot_image_check_feed(&chk, fx->data, fx->len - 1);
	zassert_equal(mcuboot_image_check_end(&chk, &info, &why), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "ended before its declared size"), "%s", why);
	zassert_mem_equal(info.sha256, zero, 32);
}

ZTEST(mcuboot_image, test_too_small)
{
	const struct fixture *fx = fixture("valid");

	/* header + vectors + TLV info is the least that can be an image */
	expect(&(struct fixture){"small", fx->data, 43, "invalid_image", "too small"},
	       run(fx->data, 43, 43));
	/* 44 bytes pass that gate and fail later */
	zassert_not_equal(run(fx->data, 44, 44), MCUBOOT_IMAGE_OK);
	zassert_is_null(strstr(why, "too small"), "%s", why);
}

ZTEST(mcuboot_image, test_null_params)
{
	const struct fixture *fx = fixture("valid");

	mcuboot_image_check_begin(&chk, NULL, (uint32_t)fx->len);
	mcuboot_image_check_feed(&chk, fx->data, fx->len);
	zassert_equal(mcuboot_image_check_end(&chk, NULL, NULL), MCUBOOT_IMAGE_INVALID);
}

ZTEST(mcuboot_image, test_abort_releases)
{
	const struct fixture *fx = fixture("valid");

	/* Many checks begun and abandoned must not exhaust anything. */
	for (int i = 0; i < 64; i++) {
		mcuboot_image_check_begin(&chk, &platform.image, (uint32_t)fx->len);
		mcuboot_image_check_feed(&chk, fx->data, 100);
		mcuboot_image_check_abort(&chk);
		mcuboot_image_check_abort(&chk);
	}
	zassert_equal(run(fx->data, fx->len, 512), MCUBOOT_IMAGE_OK);
}

ZTEST(mcuboot_image, test_result_codes)
{
	zassert_is_null(mcuboot_image_result_code(MCUBOOT_IMAGE_OK));
	zassert_str_equal(mcuboot_image_result_code(MCUBOOT_IMAGE_INVALID), "invalid_image");
	zassert_str_equal(mcuboot_image_result_code(MCUBOOT_IMAGE_UNSUPPORTED_TARGET),
			  "unsupported_target");
	zassert_is_null(mcuboot_image_result_code((enum mcuboot_image_result)7));
}

/* -- images built here --------------------------------------------------------------- */

#define BODY 600U

struct spec {
	uint16_t hdr_size;
	uint32_t flags;
	uint32_t img_size;
	uint32_t sp;
	uint32_t reset;
	/* extra bytes after the image; 0xFF */
	uint32_t trailing;
	/* false: leave the SHA-256 TLV as zeroes */
	bool rehash;
};

static const struct spec base = {
	.hdr_size = TEST_HDR,
	.img_size = BODY,
	.sp = RAM_END,
	.reset = EXEC_START + 0x401U,
	.rehash = true,
};

/* header + body + TLV info + SHA-256 TLV; returns the length */
static size_t build(const struct spec *s)
{
	const uint32_t tlv = s->hdr_size + BODY;
	size_t len = tlv + 4U + 36U + s->trailing;
	uint8_t digest[32];

	zassert_true(len <= sizeof(buf));
	memset(buf, 0, sizeof(buf));
	sys_put_le32(MCUBOOT_IMAGE_MAGIC, buf);
	sys_put_le16(s->hdr_size, &buf[8]);
	sys_put_le32(s->img_size, &buf[12]);
	sys_put_le32(s->flags, &buf[16]);
	buf[20] = 3;
	buf[21] = 1;
	sys_put_le16(9, &buf[22]);
	sys_put_le32(1000, &buf[24]);
	for (uint32_t i = 0; i < BODY; i++) {
		buf[s->hdr_size + i] = (uint8_t)(i * 13U + 1U);
	}
	sys_put_le32(s->sp, &buf[s->hdr_size]);
	sys_put_le32(s->reset, &buf[s->hdr_size + 4U]);
	sys_put_le16(MCUBOOT_TLV_INFO_MAGIC, &buf[tlv]);
	sys_put_le16(40, &buf[tlv + 2U]);
	sys_put_le16(MCUBOOT_TLV_SHA256, &buf[tlv + 4U]);
	sys_put_le16(32, &buf[tlv + 6U]);
	if (s->rehash) {
		sha256_of(buf, tlv, digest);
		memcpy(&buf[tlv + 8U], digest, 32);
	}
	memset(&buf[tlv + 40U], 0xFF, s->trailing);
	return len;
}

static enum mcuboot_image_result run_built(const struct spec *s)
{
	size_t len = build(s);

	return run(buf, len, 97);
}

static void expect_built(const struct spec *s, enum mcuboot_image_result r, const char *fragment)
{
	zassert_equal(run_built(s), r, "%s", why ? why : "");
	if (fragment == NULL) {
		zassert_is_null(why);
	} else {
		zassert_not_null(strstr(why, fragment), "'%s'", why);
	}
}

ZTEST(mcuboot_image, test_built_base_accepted)
{
	expect_built(&base, MCUBOOT_IMAGE_OK, NULL);
	zassert_str_equal(info.version, "3.1.9+1000");
	zassert_mem_equal(info.image_hash, &buf[TEST_HDR + BODY + 8U], 32);
}

ZTEST(mcuboot_image, test_every_refused_flag)
{
	static const uint32_t refused[] = {0x4, 0x8, 0x10, 0x20, 0x200, 0x400, 0x800};
	struct spec s = base;

	for (size_t i = 0; i < ARRAY_SIZE(refused); i++) {
		s.flags = refused[i];
		expect_built(&s, MCUBOOT_IMAGE_INVALID, "encrypted, compressed");
	}
	/* Flags this loader does not care about pass (ROM_FIXED, PIC). */
	s.flags = 0x100 | 0x1;
	expect_built(&s, MCUBOOT_IMAGE_OK, NULL);
}

ZTEST(mcuboot_image, test_header_size_is_the_boards)
{
	struct spec s = base;

	s.hdr_size = 0x200;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "header size");
	s.hdr_size = 0x800;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "header size");
}

ZTEST(mcuboot_image, test_header_size_before_flags)
{
	struct spec s = base;

	s.hdr_size = 0x200;
	s.flags = 0x4;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "header size");
}

ZTEST(mcuboot_image, test_image_sizes)
{
	struct spec s = base;

	/* no room for the vector table */
	s.img_size = 7;
	expect_built(&s, MCUBOOT_IMAGE_INVALID, "no vector table");
	s.img_size = 8;
	zassert_not_equal(run_built(&s), MCUBOOT_IMAGE_OK);
	zassert_is_null(strstr(why, "no vector table"), "%s", why);
	/* more body than the file holds */
	s.img_size = BODY + 37U;
	expect_built(&s, MCUBOOT_IMAGE_INVALID, "declares more bytes");
	/* exactly the TLV info header left: passes the size gate */
	s.img_size = BODY + 36U;
	zassert_not_equal(run_built(&s), MCUBOOT_IMAGE_OK);
	zassert_is_null(strstr(why, "declares more bytes"), "%s", why);
	/* a huge size must not wrap */
	s.img_size = UINT32_MAX;
	expect_built(&s, MCUBOOT_IMAGE_INVALID, "declares more bytes");
}

ZTEST(mcuboot_image, test_tlv_area_limit)
{
	struct spec s = base;

	/* TLV area 40 bytes + trailing: exactly TLV_MAX passes the gate */
	s.trailing = CONFIG_SYSTEM_IMAGE_STORE_TLV_MAX - 40U;
	expect_built(&s, MCUBOOT_IMAGE_INVALID, "bytes after the image's TLV area");
	s.trailing++;
	expect_built(&s, MCUBOOT_IMAGE_INVALID, "continues too far past the image");
}

ZTEST(mcuboot_image, test_tlv_structure)
{
	const uint32_t tlv = TEST_HDR + BODY;
	size_t len;

	/* info magic wrong */
	len = build(&base);
	sys_put_le16(0x6906, &buf[tlv]);
	zassert_equal(run(buf, len, 64), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "no TLV area after its body"), "%s", why);

	/* SHA-256 TLV of the wrong length: 28 bytes, then a 0-length entry */
	len = build(&base);
	sys_put_le16(28, &buf[tlv + 6U]);
	sys_put_le16(0x20, &buf[tlv + 36U]);
	sys_put_le16(0, &buf[tlv + 38U]);
	zassert_equal(run(buf, len, 64), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "wrong length"), "%s", why);

	/* an entry whose length runs past the area */
	len = build(&base);
	sys_put_le16(33, &buf[tlv + 6U]);
	zassert_equal(run(buf, len, 64), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "TLV area is malformed"), "%s", why);

	/* a partial entry header at the end: area 40 + 2 */
	struct spec s = base;

	s.trailing = 2;
	len = build(&s);
	sys_put_le16(42, &buf[tlv + 2U]);
	zassert_equal(run(buf, len, 64), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "TLV area is malformed"), "%s", why);

	/* a SHA-256 TLV that is not the first entry is found */
	len = build(&base);
	{
		uint8_t hash[32];

		memcpy(hash, &buf[tlv + 8U], 32);
		/* area: info(4) + unknown 0x30 len 0 (4) + sha (36) = 44 */
		sys_put_le16(44, &buf[tlv + 2U]);
		sys_put_le16(0x30, &buf[tlv + 4U]);
		sys_put_le16(0, &buf[tlv + 6U]);
		sys_put_le16(MCUBOOT_TLV_SHA256, &buf[tlv + 8U]);
		sys_put_le16(32, &buf[tlv + 10U]);
		memcpy(&buf[tlv + 12U], hash, 32);
		len += 4U;
	}
	zassert_equal(run(buf, len, 64), MCUBOOT_IMAGE_OK, "%s", why);
}

ZTEST(mcuboot_image, test_tlv_boundaries_found_by_mutation)
{
	const uint32_t tlv = TEST_HDR + BODY;
	size_t len;

	/* a protected area of 2 bytes that looks like a protected info header */
	len = build(&base);
	sys_put_le16(2, &buf[10]);
	sys_put_le16(MCUBOOT_TLV_PROT_INFO_MAGIC, &buf[tlv]);
	sys_put_le16(2, &buf[tlv + 2U]);
	zassert_equal(run(buf, len, 64), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "protected TLV area is malformed"), "%s", why);

	/* a TLV total 2 bytes past the area */
	len = build(&base);
	sys_put_le16(42, &buf[tlv + 2U]);
	zassert_equal(run(buf, len, 64), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "runs past the end of the file"), "%s", why);

	/* a SHA-256 TLV longer than 32 bytes, digest in its first 32 */
	struct spec s = base;

	s.trailing = 4;
	len = build(&s);
	sys_put_le16(44, &buf[tlv + 2U]);
	sys_put_le16(36, &buf[tlv + 6U]);
	zassert_equal(run(buf, len, 64), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "wrong length"), "%s", why);
}

ZTEST(mcuboot_image, test_protected_tlv_sizes)
{
	const struct fixture *fx = fixture("valid_prot_tlv");
	const uint16_t prot = sys_get_le16(&fx->data[10]);
	const uint32_t img = sys_get_le32(&fx->data[12]);

	zassert_true(prot > 0U);
	memcpy(buf, fx->data, fx->len);
	/* the protected info's total disagrees with the header */
	sys_put_le16(prot + 4U, &buf[TEST_HDR + img + 2U]);
	zassert_equal(run(buf, fx->len, 100), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "protected TLV area is malformed"), "%s", why);

	/* a protected area too small to hold its own info header */
	struct spec s = base;
	size_t len = build(&s);

	sys_put_le16(2, &buf[10]);
	zassert_equal(run(buf, len, 100), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "protected TLV area is malformed"), "%s", why);
}

ZTEST(mcuboot_image, test_stack_pointer_ranges)
{
	struct spec s = base;

	s.sp = RAM_START;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "stack pointer");
	s.sp = RAM_START + 1U;
	expect_built(&s, MCUBOOT_IMAGE_OK, NULL);
	s.sp = RAM_END;
	expect_built(&s, MCUBOOT_IMAGE_OK, NULL);
	s.sp = RAM_END + 1U;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "stack pointer");
	/* the second range counts */
	s.sp = RAM2_END;
	expect_built(&s, MCUBOOT_IMAGE_OK, NULL);
	s.sp = RAM2_START;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "stack pointer");
	/* unused ranges (end 0) accept nothing */
	s.sp = 0;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "stack pointer");
}

ZTEST(mcuboot_image, test_reset_vector_window)
{
	struct spec s = base;

	s.reset = EXEC_START | 1U;
	expect_built(&s, MCUBOOT_IMAGE_OK, NULL);
	s.reset = EXEC_START - 2U + 1U;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "reset vector");
	s.reset = EXEC_END - 2U + 1U;
	expect_built(&s, MCUBOOT_IMAGE_OK, NULL);
	s.reset = EXEC_END | 1U;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "reset vector");
	s.reset = EXEC_START + 0x400U;
	expect_built(&s, MCUBOOT_IMAGE_UNSUPPORTED_TARGET, "reset vector");
}

ZTEST(mcuboot_image, test_digest_outranks_vectors)
{
	struct spec s = base;

	s.sp = 0x10000000U;
	s.rehash = false;
	expect_built(&s, MCUBOOT_IMAGE_INVALID, "SHA-256 does not match");
}

ZTEST(mcuboot_image, test_digest_covers_the_header)
{
	size_t len = build(&base);

	/* the build number is in the hashed header */
	buf[27] ^= 0x01;
	zassert_equal(run(buf, len, 50), MCUBOOT_IMAGE_INVALID);
	zassert_not_null(strstr(why, "SHA-256 does not match"), "%s", why);
}
