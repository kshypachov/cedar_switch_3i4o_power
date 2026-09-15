/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * system-image-store: the one staged STM32 image, written straight into
 * MCUboot's secondary slot (slot 2).
 *
 * Contract: "Обновление STM32" in docs/device-development/api-contract.md, the
 * Upload and FirmwareImage schemas in openapi.json (target stm32u585), and the
 * design in docs/device-development/reports/stm32-update/README.md (owner's
 * decision 2026-09-15: the bytes go into slot 2, not through a file). The API
 * mirrors firmware-store's (firmware_store.h) call for call and errno for errno,
 * so the HTTP binding dispatches between the two by target with one table. The
 * shape, and why:
 *
 * - **The bytes in the slot, the state in a file.** Metadata (id, display
 *   filename, declared size and SHA-256, received bytes, outcome of the check)
 *   is `<dir>/sysimg.meta`, versioned with a CRC32 and replaced atomically
 *   (written to `sysimg.meta.tmp`, synced, renamed). The name does not start with
 *   "upload": firmware-store removes that namespace from the same directory.
 *
 * - **Creation clears the trailer.** The last `trailer_bytes` of the slot (the
 *   last sector of MCUboot's 64 KiB layout) hold the magic and flags of an
 *   earlier swap request; they are erased before the metadata exists, so a
 *   half-received image is never paired with a stale request.
 *
 * - **The slot must be free to overwrite.** While the running image is not
 *   confirmed, or a swap is already requested, slot 2 holds the firmware a
 *   revert needs (or the one about to be swapped in). The platform's
 *   slot_locked() says so; creating an upload and accepting a chunk then fail
 *   -EACCES (409 invalid_state).
 *
 * - **`received_bytes` only moves after the bytes are in flash and read back.**
 *   A chunk is accepted and copied on the HTTP thread (sys_img_chunk_accept()),
 *   and written by the worker (sys_img_chunk_commit()): every sector that
 *   *starts* inside the chunk is erased, the chunk is programmed, read back and
 *   compared, and only then the metadata names the new length. The sector in
 *   whose middle the chunk begins already holds accepted bytes and is not erased.
 *
 * - **A chunk can always be sent again.** After a reboot between the write and
 *   the metadata, the client resends from the old `received_bytes`. The bytes
 *   after it in the first sector may be partly programmed already; NOR only
 *   clears bits, so programming the same bytes over them is exact. If they
 *   cannot be programmed to the new bytes (a different chunk than the lost one),
 *   the sector is repaired: its accepted prefix is read, the sector erased and
 *   the prefix written back before the chunk.
 *
 * - **Recovery by the metadata and the slot.** Corrupt metadata is removed. A
 *   `ready` upload is re-read at init: if the slot no longer holds the image the
 *   check found (its SHA-256 TLV differs - MCUboot swapped the slots since), the
 *   upload is gone. A `receiving` upload continues from `received_bytes`.
 *
 * - **No space check.** The slot is reserved for this; the metadata is a few
 *   hundred bytes in /lfs.
 *
 * - **Expiry by uptime and a cancelled check** behave as firmware-store's: an
 *   upload untouched for CONFIG_SYSTEM_IMAGE_STORE_EXPIRE_SECONDS is removed when
 *   next asked about (not while in use, a chunk is pending or a check runs;
 *   reading its state is not activity), and a cancelled check leaves it
 *   `receiving` with every byte in place. Expiry and deletion forget the
 *   metadata; the slot is left as it is (no trailer magic, so MCUboot ignores it).
 *
 * Threads and locking: one mutex guards the state. Calls from the HTTP thread
 * (create, get, current, chunk_accept, chunk_discard, verify_begin, set_in_use,
 * set_active_job, image_info, tick) never touch the slot except create's trailer
 * erase (one 64 KiB erase); create writes the small metadata file. The worker's
 * calls (chunk_commit, verify, delete) do their flash and file I/O without the
 * lock and must come from **one** thread at a time - the binding's job worker -
 * which the pending-chunk and verifying flags make safe against the HTTP-side
 * calls. Every function returns 0 or a negative errno.
 */

#ifndef SYSTEM_IMAGE_STORE_H_
#define SYSTEM_IMAGE_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <system_image_store/mcuboot_image.h>

#ifdef __cplusplus
extern "C" {
#endif

/** "sysimg_" and 16 hex digits. */
#define SYS_IMG_ID_LEN 23
/** Bytes of the display filename: the schema allows 128 characters of UTF-8. */
#define SYS_IMG_FILENAME_MAX 512
/** A job id as job-manager formats it (JOB_ID_MAX_LEN). */
#define SYS_IMG_JOB_ID_MAX 31
#define SYS_IMG_ERROR_CODE_MAX 31
#define SYS_IMG_ERROR_MESSAGE_MAX 127

/** Upload.state. `verifying` is never stored: after a reboot it is `receiving`. */
enum sys_img_state {
	SYS_IMG_RECEIVING = 0,
	SYS_IMG_VERIFYING,
	SYS_IMG_READY,
	SYS_IMG_FAILED,
};

/** MCUboot's secondary slot and what "this board" means, from the board. */
struct sys_img_platform {
	/** Read @p len bytes at @p offset of the slot. 0 or a negative errno. */
	int (*read)(void *ctx, uint32_t offset, uint8_t *buf, size_t len);
	/** Program @p len bytes at @p offset (NOR: bits only go from 1 to 0). */
	int (*write)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
	/** Erase [@p offset, @p offset + @p len), whole sectors. */
	int (*erase)(void *ctx, uint32_t offset, uint32_t len);
	/**
	 * True while slot 2 must not be overwritten: the running image is not
	 * confirmed, or a swap is requested for the next reset.
	 */
	bool (*slot_locked)(void *ctx);
	void *ctx;
	/** Bytes of the slot; a multiple of @ref sector_size. */
	uint32_t slot_size;
	/** Erase unit of the slot as the application's driver sees it; a power of two
	 * no larger than CONFIG_SYSTEM_IMAGE_STORE_SECTOR_MAX. */
	uint32_t sector_size;
	/** MCUboot's trailer sector at the end of the slot; a multiple of @ref sector_size. */
	uint32_t trailer_bytes;
	/** Header size, RAM ranges and execution window of this board's images. */
	struct mcuboot_image_params image;
};

/** The Upload resource, plus what the binding and the updater need. */
struct sys_img_upload {
	char id[SYS_IMG_ID_LEN + 1];
	char filename[SYS_IMG_FILENAME_MAX + 1];
	uint32_t size_bytes;
	/** The next acceptable offset: bytes known to be in the slot. */
	uint32_t received_bytes;
	uint8_t sha256[32];
	enum sys_img_state state;
	/** Set by the binding; "" when none. */
	char active_job_id[SYS_IMG_JOB_ID_MAX + 1];
	/** An install holds the upload (sys_img_set_in_use()). */
	bool in_use;
	/** A chunk was accepted and is not yet committed. */
	bool chunk_pending;
	/** FirmwareImage, valid when @ref has_image (state ready). */
	bool has_image;
	char version[MCUBOOT_IMAGE_VERSION_MAX + 1];
	uint8_t image_hash[32];
	/** ErrorDetail of a failed check: the contract's code and a sentence; "" otherwise. */
	char error_code[SYS_IMG_ERROR_CODE_MAX + 1];
	char error_message[SYS_IMG_ERROR_MESSAGE_MAX + 1];
};

/** What the updater needs of a `ready` upload. */
struct sys_img_image {
	char version[MCUBOOT_IMAGE_VERSION_MAX + 1];
	uint8_t major;
	uint8_t minor;
	uint16_t revision;
	uint32_t build;
	uint8_t image_hash[32];
	uint32_t size_bytes;
};

/**
 * @brief Open the store: metadata in @p dir (created if missing), the slot
 *        through @p platform, and recover what is there.
 *
 * Safe to call again (the sim tier "reboots" by remounting and calling it). The
 * expiry period starts at @p now_ms. @p platform must outlive the store.
 *
 * @retval 0       ready; an upload may or may not exist
 * @retval -EINVAL @p dir is NULL or too long, or @p platform is incomplete or
 *                 its sizes are inconsistent
 * @retval -EIO    the directory or the slot could not be read, or PSA did not start
 */
int sys_img_init(const char *dir, const struct sys_img_platform *platform, int64_t now_ms);

/**
 * @brief Start a new upload (createUpload, target stm32u585).
 *
 * A failed upload is replaced; any other existing one must be deleted first.
 *
 * @retval 0       created; @p out describes it (may be NULL)
 * @retval -EAGAIN sys_img_init() has not succeeded
 * @retval -EINVAL empty or too long @p filename, @p size of 0, NULL @p sha256
 * @retval -EBUSY  an upload exists that is not failed, or an install uses one
 * @retval -EFBIG  @p size is more than the slot less its trailer (413)
 * @retval -EACCES the slot is locked (409 invalid_state)
 * @retval -EIO    the trailer could not be erased or the metadata written
 */
int sys_img_create(const char *filename, uint32_t size, const uint8_t sha256[32], int64_t now_ms,
		   struct sys_img_upload *out);

/**
 * @brief The upload named @p id (getUpload).
 *
 * @retval 0       @p out filled
 * @retval -ENOENT no such upload, or it expired just now
 */
int sys_img_get(const char *id, int64_t now_ms, struct sys_img_upload *out);

/**
 * @brief The upload, whatever its id - for the binding's "one upload across
 *        targets" rule.
 *
 * @retval 0       @p out filled (may be NULL)
 * @retval -ENOENT none, or it expired just now
 */
int sys_img_current(int64_t now_ms, struct sys_img_upload *out);

/**
 * @brief Take a chunk (writeUploadChunk), on the HTTP thread: check it and copy
 *        @p len bytes into the staging buffer. Nothing is written yet.
 *
 * Checked in this order (firmware-store's, with the lock second):
 *
 * @retval 0          staged; commit it on the worker, or discard it
 * @retval -ENOENT    no such upload (404)
 * @retval -EACCES    the slot is locked (409 invalid_state)
 * @retval -EINVAL    the upload is not `receiving` (409 invalid_state)
 * @retval -EBUSY     a chunk is already pending (409 busy)
 * @retval -ERANGE    @p offset is not received_bytes (409 offset_mismatch)
 * @retval -ENODATA   @p len is 0 (422 validation_failed)
 * @retval -E2BIG     @p len is more than CONFIG_SYSTEM_IMAGE_STORE_CHUNK_MAX (413)
 * @retval -EOVERFLOW the chunk runs past the declared size (422 validation_failed)
 */
int sys_img_chunk_accept(const char *id, uint32_t offset, const uint8_t *data, size_t len,
			 int64_t now_ms);

/** @brief Drop a staged chunk that will not be committed. */
void sys_img_chunk_discard(const char *id);

/**
 * @brief Write the staged chunk (worker): erase, program, read back, metadata.
 *
 * The chunk is no longer pending afterwards whatever the outcome; on failure
 * received_bytes has not moved and the client sends the same offset again -
 * except when a repair erased the sector holding accepted bytes and could not
 * write them back: received_bytes then drops to that sector's start (recorded),
 * and the client's resend meets offset_mismatch and reads the upload again.
 *
 * @retval 0        in the slot; received_bytes advanced
 * @retval -ENOENT  no such upload
 * @retval -ENODATA no chunk is pending
 * @retval -EIO     erase, write or read failed, the read-back differs, or the
 *                  metadata could not be written
 */
int sys_img_chunk_commit(const char *id, int64_t now_ms);

/**
 * @brief Claim the check (verifyUpload) on the HTTP thread: `verifying` from now.
 *
 * @retval 0       claimed; run sys_img_verify() on the worker
 * @retval -ENOENT no such upload
 * @retval -EBUSY  a chunk is pending, or an install uses the upload
 * @retval -EINVAL not all bytes received, or already `verifying` or `ready`
 */
int sys_img_verify_begin(const char *id, int64_t now_ms);

/** Asked between pieces of the check; true stops it. */
typedef bool (*sys_img_cancelled_t)(void *ctx);
/** Bytes checked so far, of @p total. */
typedef void (*sys_img_progress_t)(void *ctx, uint32_t done, uint32_t total);

/**
 * @brief Run the claimed check (worker): read the slot through mcuboot_image and
 *        compare the file's SHA-256 with the declared one.
 *
 * A declared digest that does not match is `invalid_image` and outranks what the
 * structure says. The outcome is the upload's state (`ready`, or `failed` with
 * code and message); read it with sys_img_get(). Either callback may be NULL.
 *
 * @retval 0          the check finished (whichever way)
 * @retval -ENOENT    no such upload, or no check was claimed
 * @retval -ECANCELED stopped by @p cancelled; the upload is `receiving` again
 * @retval -EIO       the slot could not be read, or the outcome not recorded;
 *                    the upload is back to its state before the claim
 */
int sys_img_verify(const char *id, sys_img_cancelled_t cancelled, sys_img_progress_t progress,
		   void *ctx, int64_t now_ms);

/**
 * @brief Forget the upload (deleteUpload, on the worker). The slot is not erased.
 *
 * @retval 0       removed
 * @retval -ENOENT no such upload
 * @retval -EBUSY  an install uses it, a chunk is pending or a check runs (409 busy)
 * @retval -EIO    the metadata could not be removed; the upload still exists
 */
int sys_img_delete(const char *id);

/**
 * @brief Mark @p id as held by an install, or release it.
 *
 * @retval -ENOENT no such upload
 * @retval -EINVAL holding one that is not `ready`
 */
int sys_img_set_in_use(const char *id, bool in_use);

/** @brief Record the job working on @p id (NULL or "" clears it). -ENOENT or 0. */
int sys_img_set_active_job(const char *id, const char *job_id);

/**
 * @brief Version, image hash and size of a `ready` upload, for the updater.
 *
 * @retval 0       @p out filled
 * @retval -ENOENT no such upload
 * @retval -EINVAL it is not `ready`
 */
int sys_img_image_info(const char *id, struct sys_img_image *out);

/** @brief Remove an upload that has expired by @p now_ms. For a periodic caller. */
void sys_img_tick(int64_t now_ms);

/** @brief Wire name of a state: "receiving", "verifying", "ready", "failed". */
const char *sys_img_state_str(enum sys_img_state state);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_IMAGE_STORE_H_ */
