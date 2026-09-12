/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A storage backend that can fail on purpose.
 *
 * The real store's power-loss behaviour cannot be demonstrated on a board
 * without actually cutting power at a chosen instruction, so the interesting
 * half of this module is only testable through injection. This fake covers the
 * three failures that matter:
 *
 *  - a write that reports an error and changes nothing;
 *  - a write that tears — the first N bytes land and the rest never do, which
 *    is what a real flash write interrupted by power loss leaves behind;
 *  - an erase that fails.
 *
 * Slot contents survive fake_storage_reboot(), which is how a reboot is
 * simulated: RAM state is dropped, storage is not.
 */

#ifndef FAKE_STORAGE_H_
#define FAKE_STORAGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <device_config_store/device_config_store.h>

#define FAKE_SLOT_CAPACITY 1024

struct fake_slot {
	bool present;
	size_t len;
	uint8_t data[FAKE_SLOT_CAPACITY];
};

struct fake_storage {
	struct fake_slot slots[DEVICE_CONFIG_SLOT_COUNT];

	/* Injection. All default to off. */
	int fail_write_slot;  /**< Slot whose next write fails, or -1. */
	int fail_write_errno; /**< Error it fails with. */
	int tear_write_slot;  /**< Slot whose next write is truncated, or -1. */
	size_t tear_after;    /**< Bytes that survive the torn write. */
	int fail_erase_slot;  /**< Slot whose next erase fails, or -1. */

	/* Observation. */
	unsigned int writes[DEVICE_CONFIG_SLOT_COUNT];
	unsigned int erases[DEVICE_CONFIG_SLOT_COUNT];
};

/** Zero every slot and clear all injection. */
void fake_storage_init(struct fake_storage *fs);

/** Fill @p backend with callbacks bound to @p fs. */
void fake_storage_bind(struct fake_storage *fs, struct device_config_backend *backend);

/** Clear injection and the write/erase counters, keeping slot contents. */
void fake_storage_reset_injection(struct fake_storage *fs);

/** Flip one byte in a slot, simulating bit rot rather than a torn write. */
void fake_storage_corrupt_byte(struct fake_storage *fs, enum device_config_slot slot,
			       size_t offset);

#endif /* FAKE_STORAGE_H_ */
