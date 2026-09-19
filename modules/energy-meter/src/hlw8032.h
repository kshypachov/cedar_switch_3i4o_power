/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HLW8032 frame (datasheet REV 1.5): 24 bytes at 4800 8E1 (55 ms), then a ~50 ms pause.
 *   0      State: 0x55 normal; 0xFx - bit 3/2/1: voltage/current/power register
 *          overflowed (the value is near 0), bit 0: parameter registers unusable;
 *          0xAA: parameter registers unusable
 *   1      0x5A
 *   2..19  voltage parameter, voltage, current parameter, current, power
 *          parameter, power - 24 bits each, high byte first
 *   20     bits 6/5/4: voltage/current/power register updated; bit 7 toggles
 *          when PF overflows
 *   21..22 PF, 16-bit count of energy pulses
 *   23     sum of bytes 2..22
 * No Zephyr dependency: tests/energy_meter runs it on native_sim.
 */

#ifndef HLW8032_H_
#define HLW8032_H_

#include <stdbool.h>
#include <stdint.h>

#define HLW8032_FRAME_LEN 24

struct hlw8032_regs {
	uint32_t v_par, v; /* voltage = v_par / v * VF; v == 0: no value */
	uint32_t i_par, i; /* current = i_par / i * CF */
	uint32_t p_par, p; /* power = p_par / p * VF * CF */
	uint16_t pf;       /* PF pulse count, wraps at 65536 */
};

/* A complete frame the values can be taken from. */
bool hlw8032_frame_ok(const uint8_t *f);

/*
 * Take the values of a frame hlw8032_frame_ok() accepted into @p r. A register
 * that was not updated keeps its previous value; one that overflowed becomes 0.
 */
void hlw8032_parse(const uint8_t *f, struct hlw8032_regs *r);

#endif /* HLW8032_H_ */
