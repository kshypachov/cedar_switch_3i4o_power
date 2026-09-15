/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everything system-updater asks the board for, in RAM.
 *
 * The updater's rules are about what a restart leaves behind: which journal
 * stage reached storage, and which image MCUboot then runs, confirmed or not.
 * The fake keeps every saved journal, so a test can "restart" at any of them
 * with fake_system_reboot_into() - the image MCUboot would run and whether it
 * is confirmed - and call system_updater_init() again. Each call can fail
 * (sticky return value) and can run a test's hook once, in the middle of it.
 */

#ifndef FAKE_SYSTEM_PLATFORM_H_
#define FAKE_SYSTEM_PLATFORM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <system_updater/system_updater.h>

enum fake_system_call {
	FSP_RUNNING = 0,
	FSP_STAGED,
	FSP_IN_USE,
	FSP_CONSUMED,
	FSP_REQUEST,
	FSP_SWAP_PENDING,
	FSP_CONFIRM,
	FSP_REBOOT,
	FSP_LOAD,
	FSP_SAVE,
	FSP_SLEEP,

	FSP_CALL_COUNT
};

#define FAKE_SYSTEM_MAX_SAVES 32

struct fake_system {
	unsigned int count[FSP_CALL_COUNT];
	/** Return value of running, staged, in_use, request, confirm, load and save. Sticky. */
	int rc[FSP_CALL_COUNT];
	/** With rc[FSP_SAVE] set: fail only the Nth save (1-based); 0: every one. */
	unsigned int save_fail_at;
	/** A failing running_image / journal_load still fills its output (a partial read). */
	bool running_fills_on_error;
	bool load_fills_on_error;

	struct system_image running;
	bool running_confirmed;

	struct system_image staged;
	uint32_t staged_size;

	bool in_use;
	char in_use_id[SYSTEM_UPDATE_UPLOAD_ID_MAX_LEN + 1];
	char consumed_id[SYSTEM_UPDATE_UPLOAD_ID_MAX_LEN + 1];

	bool swap_pending;
	/** A successful request sets swap_pending. */
	bool request_sets_pending;
	/** A successful confirm makes the running image confirmed. */
	bool confirm_takes;
	/** now_ms when request_swap and reboot were called. */
	int64_t request_at_ms;
	int64_t reboot_at_ms;

	bool has_journal;
	struct system_update_journal journal;
	struct system_update_journal saves[FAKE_SYSTEM_MAX_SAVES];
	size_t save_count;

	int64_t now_ms;
	uint32_t slept_ms;

	/** Run inside the next call of kind @ref hook_call, once. */
	void (*hook)(void *arg);
	void *hook_arg;
	enum fake_system_call hook_call;
};

extern struct fake_system fake_system;
extern const struct system_updater_platform fake_system_platform;

/** An image with every hash byte @p seed. */
struct system_image fake_system_image(uint8_t major, uint8_t minor, uint16_t revision,
				      uint32_t build, uint8_t seed);

/**
 * Every call succeeds; running 1.0.0+0 (seed 0xA0) confirmed; staged 1.1.0+0
 * (seed 0xB0), 100 000 bytes; no journal, no swap pending; requests set it and
 * confirmations take; clock at 1000.
 */
void fake_system_init(void);

/**
 * What a restart leaves: the last saved journal stays, MCUboot runs @p image
 * (@p confirmed or not) and the swap request is consumed. Calls and saves are
 * not cleared. The caller initialises the updater again.
 */
void fake_system_reboot_into(const struct system_image *image, bool confirmed);

#endif /* FAKE_SYSTEM_PLATFORM_H_ */
