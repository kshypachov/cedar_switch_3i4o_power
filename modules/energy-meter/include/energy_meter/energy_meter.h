/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * energy-meter: an HLW8032 on a receive-only UART (chosen cedar,hlw8032-uart)
 * and the energy counter in its own ZMS on the FRAM (partition "energy").
 *
 * The HLW8032 counts energy as PF pulses. The module turns the new pulses of each
 * frame into energy with the coefficients of that moment and keeps the energy in
 * uWh, saved once a second when it changed: a changed coefficient applies from
 * then on, never to energy already counted. Voltage, current and energy
 * coefficients are the settings-registry keys energy/cal/{voltage,current,energy}
 * (1.0 by default, 0.5..2.0); power takes the voltage and current ones.
 */

#ifndef ENERGY_METER_H_
#define ENERGY_METER_H_

#include <stdbool.h>
#include <stdint.h>

struct energy_reading {
	float voltage;        /* V */
	float current;        /* A */
	float power;          /* W, active */
	float apparent_power; /* VA */
	float power_factor;
	double energy_kwh;    /* since the counter was last reset */
	uint64_t pulses;      /* PF pulses counted since boot */
	uint32_t frames;      /* frames taken since boot; 0: no values yet */
};

/* Mount the counter's storage and start receiving. Call once, from main. */
int energy_meter_start(void);

/* The latest values; never blocks. */
void energy_meter_get(struct energy_reading *out);

#endif /* ENERGY_METER_H_ */
