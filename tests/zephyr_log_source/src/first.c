/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A log module whose name sorts before every other in this build, so it gets
 * source id 0: the backend must name a module whose id is 0 too (found by
 * mutation, reports/p5/notes-mutations-logs.md).
 */

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aaa_first, LOG_LEVEL_DBG);

void log_from_the_first_source(void)
{
	LOG_INF("from the first source");
}
