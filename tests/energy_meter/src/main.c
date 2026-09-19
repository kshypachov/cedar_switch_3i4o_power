/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HLW8032 frame check and parse (modules/energy-meter/src/hlw8032.c).
 */

#include <zephyr/ztest.h>

#include "hlw8032.h"

/* A frame with every register set, all three "updated" bits and PF 0x1234 */
static void make(uint8_t *f, uint8_t state)
{
	static const uint8_t body[] = {
		0x02, 0xE5, 0x68, /* voltage parameter */
		0x00, 0x08, 0x5C, /* voltage */
		0x00, 0x3E, 0x30, /* current parameter */
		0x01, 0x00, 0x00, /* current */
		0x4C, 0x4B, 0x40, /* power parameter */
		0x02, 0x00, 0x00, /* power */
		0x70,             /* updated: voltage, current, power */
		0x12, 0x34,       /* PF */
	};
	uint8_t sum = 0;

	f[0] = state;
	f[1] = 0x5a;
	memcpy(&f[2], body, sizeof(body));
	for (int i = 2; i <= 22; i++) {
		sum += f[i];
	}
	f[23] = sum;
}

ZTEST(hlw8032, test_accepts_a_normal_frame)
{
	uint8_t f[HLW8032_FRAME_LEN];
	struct hlw8032_regs r = {0};

	make(f, 0x55);
	zassert_true(hlw8032_frame_ok(f));
	hlw8032_parse(f, &r);
	zassert_equal(r.v_par, 0x02E568);
	zassert_equal(r.v, 0x00085C);
	zassert_equal(r.i_par, 0x003E30);
	zassert_equal(r.i, 0x010000);
	zassert_equal(r.p_par, 0x4C4B40);
	zassert_equal(r.p, 0x020000);
	zassert_equal(r.pf, 0x1234);
}

ZTEST(hlw8032, test_rejects_what_is_not_a_frame)
{
	uint8_t f[HLW8032_FRAME_LEN];

	make(f, 0x55);
	f[23]++;
	zassert_false(hlw8032_frame_ok(f), "bad checksum");
	make(f, 0x55);
	f[1] = 0x5b;
	zassert_false(hlw8032_frame_ok(f), "no 0x5A");
	make(f, 0xaa);
	zassert_false(hlw8032_frame_ok(f), "0xAA: parameters unusable");
	make(f, 0xf1);
	zassert_false(hlw8032_frame_ok(f), "0xFx bit 0: parameters unusable");
	make(f, 0x12);
	zassert_false(hlw8032_frame_ok(f), "not a state value");
	make(f, 0xf0);
	zassert_true(hlw8032_frame_ok(f));
}

ZTEST(hlw8032, test_an_overflowed_register_is_zero)
{
	uint8_t f[HLW8032_FRAME_LEN];
	struct hlw8032_regs r = {0};

	make(f, 0xfe); /* voltage, current and power overflowed */
	zassert_true(hlw8032_frame_ok(f));
	hlw8032_parse(f, &r);
	zassert_equal(r.v, 0);
	zassert_equal(r.i, 0);
	zassert_equal(r.p, 0);
	make(f, 0xf2); /* power only: no load */
	hlw8032_parse(f, &r);
	zassert_equal(r.v, 0x00085C);
	zassert_equal(r.p, 0);
}

ZTEST(hlw8032, test_a_register_not_updated_keeps_its_value)
{
	uint8_t f[HLW8032_FRAME_LEN];
	struct hlw8032_regs r = {.v = 1, .i = 2, .p = 3};

	make(f, 0x55);
	f[23] -= f[20];
	f[20] = 0;
	zassert_true(hlw8032_frame_ok(f));
	hlw8032_parse(f, &r);
	zassert_equal(r.v, 1);
	zassert_equal(r.i, 2);
	zassert_equal(r.p, 3);
}

ZTEST(hlw8032, test_pf_difference_across_the_wrap)
{
	/* energy_meter.c adds (uint16_t)(pf - last_pf) */
	zassert_equal((uint16_t)(0x0005 - 0xfffe), 7);
	zassert_equal((uint16_t)(0x1234 - 0x1234), 0);
}

ZTEST_SUITE(hlw8032, NULL, NULL, NULL, NULL, NULL);
