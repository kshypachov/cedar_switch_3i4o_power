/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See boot_streak.h.
 */

#include "boot_streak.h"

bool boot_streak_step(uint32_t stored, uint32_t threshold, uint32_t *next)
{
	/* Saturating: a count past the threshold is still past it. */
	*next = (stored == UINT32_MAX) ? stored : stored + 1U;

	return *next >= threshold;
}
