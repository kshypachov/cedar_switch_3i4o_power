/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fw_md5: the RFC 1321 test suite, the block boundaries (values from Python's
 * hashlib) and streaming in arbitrary splits.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <firmware_store/fw_md5.h>

static void hex(const uint8_t *digest, char *out)
{
	for (int i = 0; i < 16; i++) {
		static const char digits[] = "0123456789abcdef";

		out[i * 2] = digits[digest[i] >> 4];
		out[i * 2 + 1] = digits[digest[i] & 0xF];
	}
	out[32] = '\0';
}

static void assert_md5(const uint8_t *data, size_t len, const char *expected)
{
	uint8_t digest[16];
	char text[33];

	fw_md5(data, len, digest);
	hex(digest, text);
	zassert_str_equal(text, expected, "length %zu", len);
}

ZTEST(fw_md5, test_rfc1321_suite)
{
	static const struct {
		const char *in;
		const char *md5;
	} v[] = {
		{"", "d41d8cd98f00b204e9800998ecf8427e"},
		{"a", "0cc175b9c0f1b6a831c399e269772661"},
		{"abc", "900150983cd24fb0d6963f7d28e17f72"},
		{"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
		{"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
		{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
		 "d174ab98d277d9f5a5611c2c9f419d9f"},
		{"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
		 "57edf4a22be3c955ac49da2e2107b67a"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(v); i++) {
		assert_md5((const uint8_t *)v[i].in, strlen(v[i].in), v[i].md5);
	}
}

ZTEST(fw_md5, test_block_boundaries)
{
	static uint8_t a[65];
	static uint8_t ramp[1024];

	memset(a, 'a', sizeof(a));
	for (size_t i = 0; i < sizeof(ramp); i++) {
		ramp[i] = (uint8_t)i;
	}
	assert_md5(a, 55, "ef1772b6dff9a122358552954ad0df65");
	assert_md5(a, 56, "3b0c8ac703f828b04c6c197006d17218");
	assert_md5(a, 63, "b06521f39153d618550606be297466d5");
	assert_md5(a, 64, "014842d480b571495a4a0363793f7367");
	assert_md5(a, 65, "c743a45e0d2e6a95cb859adae0248435");
	assert_md5(ramp, sizeof(ramp), "b2ea9f7fcea831a4a63b213f41a8855b");
}

ZTEST(fw_md5, test_streaming_in_any_split)
{
	static uint8_t ramp[1024];
	uint8_t whole[16];

	for (size_t i = 0; i < sizeof(ramp); i++) {
		ramp[i] = (uint8_t)(i * 7);
	}
	fw_md5(ramp, sizeof(ramp), whole);

	for (size_t split = 1; split < 200; split += 13) {
		struct fw_md5 ctx;
		uint8_t digest[16];

		fw_md5_init(&ctx);
		for (size_t off = 0; off < sizeof(ramp); off += split) {
			fw_md5_update(&ctx, &ramp[off], MIN(split, sizeof(ramp) - off));
		}
		fw_md5_final(&ctx, digest);
		zassert_mem_equal(digest, whole, sizeof(whole), "split %zu", split);
	}
}

ZTEST_SUITE(fw_md5, NULL, NULL, NULL, NULL, NULL);
