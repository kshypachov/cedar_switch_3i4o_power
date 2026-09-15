/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * system-updater: the STM32 install through MCUboot's slot 2 as one job, and
 * what happens after the swap.
 *
 * Contract: api-contract.md "Обновление STM32" (install phases, SystemFirmware,
 * SystemUpdateSummary states), reports/stm32-update/README.md ("Дизайн", owner's
 * decisions: self-confirmation after 20 minutes, a console command to confirm
 * at once, downgrade only when acknowledged). The shape, and why:
 *
 * - **One job, run on the caller's worker.** The HTTP binding checks the
 *   request, creates a `system_update` job (cancellable), calls
 *   system_updater_start() and has its worker call system_updater_run().
 *   Nothing here owns a thread, as in coprocessor-updater: the sim tier drives
 *   it step by step.
 *
 * - **Phases are the contract's**: `preparing` (the staged image is still the
 *   one accepted, the running image is still confirmed), `requesting` (journal,
 *   then MCUboot's test swap), `rebooting` (journal, a short delay, restart).
 *   The job never ends on this side of the restart: after it the job is gone
 *   (404) and the outcome is last_update.
 *
 * - **Cancellable until `requesting`, never after.** job_set_cancellable() at
 *   the gate also reports a cancel that landed first, so a cancel either stops
 *   the install before the slot's trailer is touched or is refused. A cancelled
 *   install leaves last_update as it was.
 *
 * - **The journal is what makes the swap readable.** MCUboot tells the
 *   application nothing about what it did; the updater records, before each
 *   step, the stage it reached and the TLV hashes of the image that ran (`from`)
 *   and the image it asked for (`to`). At the next start system_updater_init()
 *   compares them with the running image's hash and confirmation:
 *
 *   | journal stage | running image           | last_update             |
 *   |---------------|-------------------------|-------------------------|
 *   | preparing     | any                     | interrupted             |
 *   | requesting    | the new one (see below) | as `rebooting`          |
 *   | requesting    | otherwise               | interrupted             |
 *   | rebooting     | the new one, unconfirmed| awaiting_confirmation   |
 *   | rebooting     | the new one, confirmed  | succeeded               |
 *   | rebooting     | otherwise               | failed                  |
 *   | booted        | `to`, unconfirmed       | awaiting_confirmation   |
 *   | booted        | `to`, confirmed         | succeeded               |
 *   | booted        | otherwise               | rolled_back             |
 *
 *   "The new one" is the `to` hash, and - when `to` and `from` are the same
 *   image, a reinstall - an unconfirmed one: MCUboot leaves a swapped-in test
 *   image unconfirmed, so a confirmed identical image means no swap happened.
 *   The first start of the new image writes stage `booted`: a later revert
 *   then reads `rolled_back`, not `failed`. A `requesting` journal with the old
 *   image cannot tell "the request never reached flash" from "MCUboot rejected
 *   it"; it reads `interrupted`, the likely one.
 *
 * - **The upload is consumed by the swap.** Once a swap may have happened
 *   (stage `requesting` or later) slot 2 no longer holds the bytes the upload
 *   describes - it holds the previous firmware, which a revert needs - so init
 *   tells the platform to forget the upload (upload_consumed()).
 *
 * - **Self-confirmation.** An unconfirmed running image - after an update, or
 *   put there by hand - gets a deadline of CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS
 *   from init. system_updater_tick() confirms when it passes;
 *   system_updater_confirm_now() confirms at once (the console command). A
 *   failed confirmation is retried with a doubling delay. Until the running
 *   image is confirmed no install is accepted (-EACCES): slot 2 holds the image
 *   a revert would restore.
 *
 * - **Downgrade** needs acknowledge_downgrade (-EDOM otherwise); the same
 *   version is a reinstall and allowed.
 *
 * Threads: system_updater_run() on one worker; system_updater_tick() and
 * system_updater_confirm_now() from any one thread each (they share a mutex
 * for the confirmation itself); everything else from any thread, never waiting
 * for flash (a mutex held only for copies). Every function returns 0 or a
 * negative errno.
 */

#ifndef SYSTEM_UPDATER_H_
#define SYSTEM_UPDATER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <job_manager/job_manager.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MCUboot's image version, as in the image header. */
struct system_image_version {
	uint8_t major;
	uint8_t minor;
	uint16_t revision;
	uint32_t build;
};

/** An image: its version and its TLV SHA-256 (MCUboot's hash of header and body). */
struct system_image {
	struct system_image_version version;
	uint8_t hash[32];
};

/** The contract's install phases, in order. */
enum system_update_phase {
	SYSTEM_UPDATE_PREPARING = 0,
	SYSTEM_UPDATE_REQUESTING,
	SYSTEM_UPDATE_REBOOTING,

	SYSTEM_UPDATE_PHASE_COUNT
};

/** SystemUpdateSummary.state. */
enum system_update_outcome {
	SYSTEM_UPDATE_AWAITING_CONFIRMATION = 0,
	SYSTEM_UPDATE_SUCCEEDED,
	SYSTEM_UPDATE_FAILED,
	SYSTEM_UPDATE_ROLLED_BACK,
	SYSTEM_UPDATE_INTERRUPTED,

	SYSTEM_UPDATE_OUTCOME_COUNT
};

/** How far an install got, as the journal records it. */
enum system_update_stage {
	/** No install in flight. */
	SYSTEM_UPDATE_STAGE_NONE = 0,
	/** Accepted or preparing: nothing on flash changed. */
	SYSTEM_UPDATE_STAGE_PREPARING,
	/** About to ask MCUboot for the swap; the request may or may not be on flash. */
	SYSTEM_UPDATE_STAGE_REQUESTING,
	/** The request is on flash; the restart follows. */
	SYSTEM_UPDATE_STAGE_REBOOTING,
	/** The new image has started at least once and is not confirmed yet. */
	SYSTEM_UPDATE_STAGE_BOOTED,

	SYSTEM_UPDATE_STAGE_COUNT
};

#define SYSTEM_UPDATE_UPLOAD_ID_MAX_LEN 64
#define SYSTEM_UPDATE_CODE_MAX_LEN      31
#define SYSTEM_UPDATE_MESSAGE_MAX_LEN   127
/** "255.255.65535+4294967295" and a terminator. */
#define SYSTEM_UPDATE_VERSION_STR_LEN   25

/** SystemUpdateSummary. */
struct system_update_summary {
	char job_id[JOB_ID_MAX_LEN + 1];
	enum system_update_outcome state;
	bool has_from_version;
	struct system_image_version from_version;
	bool has_version;
	struct system_image_version version;
	/** Every outcome but awaiting_confirmation and succeeded carries an error. */
	bool has_error;
	char error_code[SYSTEM_UPDATE_CODE_MAX_LEN + 1];
	char error_message[SYSTEM_UPDATE_MESSAGE_MAX_LEN + 1];
	bool error_retryable;
};

/** What the board persists; opaque to it except for its size. */
struct system_update_journal {
	/** Format number; a mismatch reads as no journal. */
	uint32_t format;
	enum system_update_stage stage;
	char job_id[JOB_ID_MAX_LEN + 1];
	char upload_id[SYSTEM_UPDATE_UPLOAD_ID_MAX_LEN + 1];
	/** The image that ran when the install was accepted. */
	struct system_image from;
	/** The image the install asked for. */
	struct system_image to;
	bool has_last;
	struct system_update_summary last;
};

#define SYSTEM_UPDATE_JOURNAL_FORMAT 1

/**
 * Everything the updater needs from the board. All are required. Each returns
 * 0 or a negative errno unless it says otherwise.
 */
struct system_updater_platform {
	/**
	 * The image in slot 1 (the one running) and whether MCUboot considers it
	 * confirmed. -EIO when its header or hash TLV cannot be read.
	 */
	int (*running_image)(void *ctx, struct system_image *out, bool *confirmed);
	/**
	 * The verified image of @p upload_id in slot 2 and its size in bytes.
	 * -ENOENT no such upload; -EINVAL it is not `ready`.
	 */
	int (*staged_image)(void *ctx, const char *upload_id, struct system_image *out,
			    uint32_t *size);
	/** Hold the upload for the install, or release it. -ENOENT/-EINVAL as above. */
	int (*set_upload_in_use)(void *ctx, const char *upload_id, bool in_use);
	/** Slot 2 no longer holds this upload's bytes: forget it. */
	void (*upload_consumed)(void *ctx, const char *upload_id);
	/** Ask MCUboot for a test swap at the next reset (boot_request_upgrade). */
	int (*request_swap)(void *ctx);
	/** MCUboot will swap at the next reset (reads slot 2's trailer). */
	bool (*swap_pending)(void *ctx);
	/** Mark the running image confirmed (boot_write_img_confirmed). */
	int (*confirm)(void *ctx);
	/** Restart the device. Never returns on the board. */
	void (*reboot)(void *ctx);
	/** -ENOENT when there is no journal; any error reads as none. */
	int (*journal_load)(void *ctx, struct system_update_journal *out);
	int (*journal_save)(void *ctx, const struct system_update_journal *journal);
	int64_t (*now_ms)(void *ctx);
	void (*sleep_ms)(void *ctx, uint32_t ms);
	void *ctx;
};

/** A snapshot for GET /system/firmware and the shell. */
struct system_updater_state {
	/** An install is accepted or running (until the restart). */
	bool active;
	enum system_update_phase phase;
	char job_id[JOB_ID_MAX_LEN + 1];
	char upload_id[SYSTEM_UPDATE_UPLOAD_ID_MAX_LEN + 1];
	/** False when the running image could not be read at init. */
	bool running_known;
	struct system_image running;
	bool running_confirmed;
	/** The running image is unconfirmed and will confirm itself. */
	bool confirm_pending;
	/** Seconds until then, rounded up; 0 once due. Valid with confirm_pending. */
	uint32_t confirm_remaining_seconds;
	bool swap_pending;
	bool has_last;
	struct system_update_summary last;
};

/**
 * @brief Take @p platform, read the journal and the running image and
 *        reconcile them (see the table above); arm self-confirmation when the
 *        running image is unconfirmed. A changed journal is saved.
 *
 * When the running image cannot be read the journal is left untouched and no
 * install is accepted (system_updater_check() -EIO).
 *
 * @retval 0       ready
 * @retval -EINVAL a required function is missing
 * @retval -EBUSY  system_updater_run() is executing
 */
int system_updater_init(const struct system_updater_platform *platform);

/**
 * @brief Would an install be accepted now? Cheap, for the HTTP binding before
 *        it creates a job; never touches flash.
 *
 * @retval 0       yes
 * @retval -EAGAIN system_updater_init() has not run
 * @retval -EBUSY  an install is active (409 busy)
 * @retval -EIO    the running image could not be read at init
 * @retval -EACCES the running image is not confirmed, or a swap is pending
 *                 (409 invalid_state)
 */
int system_updater_check(void);

/**
 * @brief Accept the install of @p upload_id as job @p job_id (created by the
 *        caller, kind system_update, cancellable) and save the journal. The
 *        work happens in system_updater_run().
 *
 * @retval 0       accepted
 * @retval -EAGAIN, -EBUSY, -EIO, -EACCES as system_updater_check()
 * @retval -EINVAL an empty or overlong id
 * @retval -ENOENT no such upload (404)
 * @retval -ENODATA the upload is not `ready` (409 invalid_state)
 * @retval -EDOM   the image's version is lower than the running one and
 *                 @p acknowledge_downgrade is false (422 validation_failed)
 * @retval -ENOSPC the journal could not be saved; nothing was accepted (500)
 */
int system_updater_start(const char *upload_id, const char *job_id, bool acknowledge_downgrade);

/**
 * @brief Run the accepted install on the calling thread, up to the restart.
 *        Returns at once when nothing was accepted; on the board it does not
 *        return after `rebooting`.
 */
void system_updater_run(void);

/**
 * @brief Confirm the running image when its deadline has passed (and a
 *        failed attempt's retry delay too). For a periodic caller.
 */
void system_updater_tick(int64_t now_ms);

/**
 * @brief Confirm the running image now (the console command).
 *
 * @retval 0       confirmed (or already was)
 * @retval -EAGAIN not initialised
 * @retval -EIO    the running image could not be read, or the confirmation
 *                 failed or did not take
 */
int system_updater_confirm_now(void);

/** @brief Never waits for flash or for a run in progress. */
void system_updater_get_state(struct system_updater_state *out);

/** @brief <0, 0 or >0 as @p a is lower, equal or higher: major, minor, revision, build. */
int system_image_version_cmp(const struct system_image_version *a,
			     const struct system_image_version *b);

/** @brief "major.minor.revision+build" into @p buf; the length written, or -ENOSPC. */
int system_image_version_str(const struct system_image_version *v, char *buf, size_t cap);

/** @brief Wire name, e.g. "requesting". NULL if out of range. */
const char *system_update_phase_str(enum system_update_phase phase);

/** @brief Wire name, e.g. "rolled_back". NULL if out of range. */
const char *system_update_outcome_str(enum system_update_outcome outcome);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_UPDATER_H_ */
