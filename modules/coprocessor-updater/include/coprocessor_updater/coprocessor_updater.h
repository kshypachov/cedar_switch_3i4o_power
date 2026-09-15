/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * coprocessor-updater: the UART install of an ESP32-C6 image as one job.
 *
 * Contract: api-contract.md "Upload и ESP32 update" (install phases,
 * `succeeded` only after reset, reconnect and a confirmed version,
 * recovery_required, `interrupted` after a restart and no automatic
 * continuation), development plan section 3 (module row) and section 8
 * ("Условия и отказы"), reports/p6/README.md ("Дизайн", owner's decisions №1,
 * №2 and №3). The shape, and why:
 *
 * - **One job, run to the end on the caller's worker.** The HTTP binding
 *   checks the request, creates a `coprocessor_update` job, calls
 *   coprocessor_updater_start() and has a worker call
 *   coprocessor_updater_run(). Nothing here owns a thread: the board already
 *   has a worker, and a synchronous run is what the sim tier can drive step by
 *   step.
 *
 * - **Phases are the contract's**: preflight, entering_bootloader, begin,
 *   writing, verifying, reconnecting, health_check, complete. `activating` and
 *   `confirming` are not used in the UART path. Progress is per phase, in
 *   bytes for writing.
 *
 * - **Cancellable until `begin`, never after.** The job is created
 *   cancellable; at `begin` the updater turns that off with
 *   job_set_cancellable(), which also reports a cancel that landed first - so
 *   a cancel either stops the install before the ROM erases anything or is
 *   refused with 409 invalid_state. A cancelled install leaves last_update as
 *   it was: nothing on the chip changed.
 *
 * - **recovery_required means "the chip's flash may no longer boot".** Any
 *   failure from `begin` on sets it; failures before (the UART is busy, the
 *   ROM loader did not answer, the image cannot be opened) do not.
 *
 * - **A journal survives the STM32.** It is saved when an install starts and
 *   at every phase change, and it holds the last outcome. On init an install
 *   the journal still calls active becomes last_update `interrupted`, with
 *   recovery_required when it had reached `begin`, and is never continued:
 *   repeating it is a new request by a person.
 *
 * - **Health is a confirmed version** (owner's decision №2 (a)). After the
 *   normal boot the console is watched for the ESP-IDF application starting -
 *   an early sign, recorded, not decisive - and then the platform restarts
 *   ESP-Hosted over the patched driver and asks for the firmware version. A
 *   version means `succeeded`. A Wi-Fi device that has not been ready since
 *   the STM32 started (an unflashed C6 at boot, board B) cannot be revived
 *   without an STM32 restart (reports/p6/notes-design-inputs.md): when the
 *   transport still answers with a version, the install succeeds and says so
 *   in stm32_restart_needed; without a version it fails.
 *
 * - **The C6's state does not gate an install.** An `offline` or `failed`
 *   coprocessor is exactly the one that needs the image. The updater only
 *   refuses a second install and a UART the USB bridge holds.
 *
 * Threads: coprocessor_updater_run() on one worker; every other function may
 * be called from any thread and never waits for the run (a mutex held only
 * for copies).
 */

#ifndef COPROCESSOR_UPDATER_H_
#define COPROCESSOR_UPDATER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <job_manager/job_manager.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The contract's install phases that the UART path uses, in order. */
enum coprocessor_update_phase {
	COPROCESSOR_UPDATE_PREFLIGHT = 0,
	COPROCESSOR_UPDATE_ENTERING_BOOTLOADER,
	COPROCESSOR_UPDATE_BEGIN,
	COPROCESSOR_UPDATE_WRITING,
	COPROCESSOR_UPDATE_VERIFYING,
	COPROCESSOR_UPDATE_RECONNECTING,
	COPROCESSOR_UPDATE_HEALTH_CHECK,
	COPROCESSOR_UPDATE_COMPLETE,

	COPROCESSOR_UPDATE_PHASE_COUNT
};

/** UpdateSummary.state. */
enum coprocessor_update_outcome {
	COPROCESSOR_UPDATE_SUCCEEDED = 0,
	COPROCESSOR_UPDATE_FAILED,
	COPROCESSOR_UPDATE_INTERRUPTED,
};

/** Longest upload id the contract allows. */
#define COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN 64
#define COPROCESSOR_UPDATE_VERSION_MAX_LEN   32
#define COPROCESSOR_UPDATE_CODE_MAX_LEN      31
#define COPROCESSOR_UPDATE_MESSAGE_MAX_LEN   127

/** Why a step failed. The strings are static; the summary copies them. */
struct coprocessor_update_error {
	/** ErrorDetail code from the contract's table. */
	const char *code;
	const char *message;
	bool retryable;
};

/** UpdateSummary, plus what the device learned on the way. */
struct coprocessor_update_summary {
	char job_id[JOB_ID_MAX_LEN + 1];
	enum coprocessor_update_outcome state;
	/** The firmware version the C6 confirmed; empty when none (null on the wire). */
	char version[COPROCESSOR_UPDATE_VERSION_MAX_LEN + 1];
	bool recovery_required;
	/** An error accompanies every outcome but `succeeded`. */
	bool has_error;
	char error_code[COPROCESSOR_UPDATE_CODE_MAX_LEN + 1];
	char error_message[COPROCESSOR_UPDATE_MESSAGE_MAX_LEN + 1];
	bool error_retryable;
	/** The last phase the install entered. */
	enum coprocessor_update_phase phase;
	/** The console saw the ESP-IDF application start after the normal boot. */
	bool uart_evidence;
	/** Wi-Fi comes up only after an STM32 restart (see the header comment). */
	bool stm32_restart_needed;
};

/** What the board persists; opaque to it except for its size. */
struct coprocessor_update_journal {
	/** CONFIG-independent format number; a mismatch reads as no journal. */
	uint32_t format;
	bool active;
	enum coprocessor_update_phase phase;
	char job_id[JOB_ID_MAX_LEN + 1];
	char upload_id[COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN + 1];
	bool has_last;
	struct coprocessor_update_summary last;
};

#define COPROCESSOR_UPDATE_JOURNAL_FORMAT 1

/**
 * The flasher, as the updater uses it. On the board this is esp-loader-adapter
 * (esp_loader_adapter_updater_loader()); the sim tier fakes it. Each returns 0
 * or a negative errno and fills @p err on failure. After a successful open()
 * the updater always calls close(), which must hand the UART back to the
 * console whatever happened; a loader may already have closed the session
 * itself when a step failed (esp-loader-adapter does, so that the flasher's
 * UART handler never outlives the call), and close() is then a no-op.
 */
struct coprocessor_updater_loader {
	/** Take the UART, reach the ROM loader, check the chip. Cleans up itself on failure. */
	int (*open)(void *ctx, struct coprocessor_update_error *err);
	/** Start writing @p size bytes at 0x0 (the ROM erases under the write). */
	int (*begin)(void *ctx, uint32_t size, struct coprocessor_update_error *err);
	int (*write)(void *ctx, const uint8_t *data, size_t len, struct coprocessor_update_error *err);
	/** Last block and MD5 verification. */
	int (*finish)(void *ctx, struct coprocessor_update_error *err);
	/** Normal boot, UART back to the console. 0 or the first failed step's errno. */
	int (*close)(void *ctx);
	void *ctx;
};

/**
 * Everything else the updater needs from the board. All are required.
 */
struct coprocessor_updater_platform {
	const struct coprocessor_updater_loader *loader;
	/**
	 * Open the staged image of @p upload_id for reading and report its size.
	 * -ENOENT no such upload; -EBUSY/-EINVAL it is not `ready`.
	 */
	int (*image_open)(void *ctx, const char *upload_id, uint32_t *size);
	/** Read @p len bytes at @p offset: the count read (> 0) or a negative errno. */
	int (*image_read)(void *ctx, uint32_t offset, uint8_t *buf, size_t len);
	void (*image_close)(void *ctx);
	/** -ENOENT when there is no journal; any other error is treated the same. */
	int (*journal_load)(void *ctx, struct coprocessor_update_journal *out);
	int (*journal_save)(void *ctx, const struct coprocessor_update_journal *journal);
	/**
	 * Forget earlier console evidence: called just before the normal boot, so
	 * boot_evidence() only reports what this boot printed.
	 */
	void (*boot_evidence_arm)(void *ctx);
	/**
	 * True once the console has seen the ESP-IDF application start since
	 * boot_evidence_arm(). Copies the application version if it was printed
	 * (empty otherwise). Must not block.
	 */
	bool (*boot_evidence)(void *ctx, char *app_version, size_t cap);
	/**
	 * Restart ESP-Hosted over the running firmware and read its version,
	 * waiting at most @p timeout_ms.
	 *
	 * @retval 0          the transport answered, @p version holds its version
	 *                    and Wi-Fi was brought up again
	 * @retval -ENODEV    the Wi-Fi device has not been ready since the STM32
	 *                    started; @p version is filled if the transport answered
	 * @retval -ETIMEDOUT no answer
	 */
	int (*transport_restart)(void *ctx, uint32_t timeout_ms, char *version, size_t cap);
	int64_t (*now_ms)(void *ctx);
	void (*sleep_ms)(void *ctx, uint32_t ms);
	void *ctx;
};

/** A snapshot for coprocessor/status and the job binding. */
struct coprocessor_updater_state {
	bool active;
	enum coprocessor_update_phase phase;
	char job_id[JOB_ID_MAX_LEN + 1];
	char upload_id[COPROCESSOR_UPDATE_UPLOAD_ID_MAX_LEN + 1];
	bool has_last;
	struct coprocessor_update_summary last;
};

/**
 * @brief Take @p platform and read the journal. An install the journal still
 *        calls active becomes last_update `interrupted` and is saved so.
 *
 * @retval 0       ready
 * @retval -EINVAL a required function is missing
 * @retval -EBUSY  an install is running
 */
int coprocessor_updater_init(const struct coprocessor_updater_platform *platform);

/**
 * @brief Would an install be accepted now? Cheap, for the HTTP binding before
 *        it creates a job.
 *
 * @retval 0       yes
 * @retval -EBUSY  an install is active, or the UART belongs to the USB bridge
 * @retval -EAGAIN coprocessor_updater_init() has not run
 */
int coprocessor_updater_check(void);

/**
 * @brief Accept the install of @p upload_id as job @p job_id (created by the
 *        caller, kind coprocessor_update, cancellable) and save the journal.
 *        The work happens in coprocessor_updater_run().
 *
 * @retval 0       accepted
 * @retval -EBUSY  as coprocessor_updater_check()
 * @retval -EINVAL an empty or overlong id
 * @retval -EIO    the journal could not be saved; nothing was accepted
 * @retval -EAGAIN not initialised
 */
int coprocessor_updater_start(const char *upload_id, const char *job_id);

/**
 * @brief Run the accepted install to its end on the calling thread. Returns at
 *        once when nothing was accepted.
 */
void coprocessor_updater_run(void);

/** @brief Never waits for a run in progress. */
void coprocessor_updater_get_state(struct coprocessor_updater_state *out);

/** @brief Wire name, e.g. "entering_bootloader". NULL if out of range. */
const char *coprocessor_update_phase_str(enum coprocessor_update_phase phase);

/** @brief Wire name, e.g. "interrupted". NULL if out of range. */
const char *coprocessor_update_outcome_str(enum coprocessor_update_outcome outcome);

#ifdef __cplusplus
}
#endif

#endif /* COPROCESSOR_UPDATER_H_ */
