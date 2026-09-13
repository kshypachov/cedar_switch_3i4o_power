/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The physical recovery of the network configuration: five power cycles in a
 * row (owner's decision, plan section 13).
 *
 * A boot counts towards the streak until it has stayed up for
 * BOOT_STREAK_WINDOW_SECONDS; then the count is cleared. The boot that reaches
 * BOOT_STREAK_THRESHOLD restores the factory network configuration — Ethernet
 * on DHCP, automatic DNS, Wi-Fi disabled with its password cleared — and
 * leaves Matter fabrics and the administrator alone
 * (network_manager_restore_defaults()).
 *
 * The owner accepted the consequence: a crash that reboots the device sooner
 * than the window five times in a row restores the network configuration too.
 *
 * Pure: the caller reads and writes the stored count. The rule is here so the
 * sim tier can check it without a settings backend.
 */

#ifndef BOOT_STREAK_H_
#define BOOT_STREAK_H_

#include <stdbool.h>
#include <stdint.h>

/** Boots in a row that restore the factory network configuration. */
#define BOOT_STREAK_THRESHOLD      5
/** Seconds a boot has to stay up to stop counting. */
#define BOOT_STREAK_WINDOW_SECONDS 30

/**
 * @brief Count this boot.
 *
 * @param stored     The count the previous boots left.
 * @param threshold  Boots that complete the streak.
 * @param next       Receives the count to store before anything else happens:
 *                   written first, it survives a power loss during the restore,
 *                   and the next boot restores again.
 * @return true when this boot completes the streak.
 */
bool boot_streak_step(uint32_t stored, uint32_t threshold, uint32_t *next);

#endif /* BOOT_STREAK_H_ */
