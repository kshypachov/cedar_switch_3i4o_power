/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hlw8032.h"

#define REG24(f, i) (((uint32_t)(f)[i] << 16) | ((uint32_t)(f)[(i) + 1] << 8) | (f)[(i) + 2])

bool hlw8032_frame_ok(const uint8_t *f)
{
	uint8_t sum = 0;

	/* 0x55, or 0xFx without the "parameters unusable" bit; 0xAA and the rest are not */
	if (f[1] != 0x5a || (f[0] != 0x55 && (f[0] & 0xf1) != 0xf0)) {
		return false;
	}
	for (int i = 2; i <= 22; i++) {
		sum += f[i];
	}
	return sum == f[23];
}

void hlw8032_parse(const uint8_t *f, struct hlw8032_regs *r)
{
	const uint8_t overflow = (f[0] & 0xf0) == 0xf0 ? f[0] : 0;
	const uint8_t updated = f[20];

	r->v_par = REG24(f, 2);
	r->i_par = REG24(f, 8);
	r->p_par = REG24(f, 14);
	if (overflow & (1u << 3)) {
		r->v = 0;
	} else if (updated & (1u << 6)) {
		r->v = REG24(f, 5);
	}
	if (overflow & (1u << 2)) {
		r->i = 0;
	} else if (updated & (1u << 5)) {
		r->i = REG24(f, 11);
	}
	if (overflow & (1u << 1)) {
		r->p = 0;
	} else if (updated & (1u << 4)) {
		r->p = REG24(f, 17);
	}
	r->pf = (uint16_t)((f[21] << 8) | f[22]);
}
