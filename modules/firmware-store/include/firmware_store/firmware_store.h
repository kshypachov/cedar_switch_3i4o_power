/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * firmware-store: the one staged ESP32-C6 image, as a file on LittleFS.
 *
 * Contract: section 8 of the development plan ("Хранение образа"), "Upload и
 * ESP32 update" in docs/device-development/api-contract.md, the Upload schema in
 * openapi.json, and the P6 design (reports/p6/README.md). The shape, and why:
 *
 * - **One upload, two files.** `<dir>/upload.bin` holds the bytes and
 *   `<dir>/upload.meta` the state (id, display filename, declared size and
 *   SHA-256, received bytes, outcome of the check), with a version and a CRC32.
 *   Names on the file system are only these; the client's filename is a value in
 *   the metadata and never part of a path. The metadata is replaced atomically
 *   (written to `upload.meta.tmp`, synced, renamed).
 *
 * - **`received_bytes` only moves after the data is on storage.** A chunk is
 *   accepted and copied on the HTTP thread (fw_store_chunk_accept()), written,
 *   synced and recorded by the worker (fw_store_chunk_commit()): data first,
 *   then the metadata naming the new length. A crash between the two leaves the
 *   file longer than the metadata says, and fw_store_init() cuts it back; so the
 *   offset a client resumes from is always one whose bytes survived.
 *
 * - **Recovery by the metadata, not by what happens to be in the directory.**
 *   Corrupt or unknown metadata removes both files. Files of this store's
 *   namespace (names starting "upload") that are not the two above are leftovers
 *   of earlier attempts and are removed; other names in the directory - the
 *   updater's journal lives there - are left alone.
 *
 * - **Space is checked, not reserved.** LittleFS cannot set blocks aside for a
 *   file. fw_store_create() refuses (`storage_full`) unless the whole declared
 *   size plus CONFIG_FIRMWARE_STORE_RESERVE_KIB is free, and every commit checks
 *   again for what is still missing, so a volume that another writer filled in
 *   the meantime fails the chunk's job rather than a write halfway.
 *
 * - **Expiry by uptime.** An upload untouched (created, a chunk accepted or
 *   committed, a check run) for CONFIG_FIRMWARE_STORE_EXPIRE_SECONDS is removed
 *   the next time the store is asked about it - unless an install uses it, a
 *   chunk is pending or a check runs. Reading its state is not activity: a
 *   forgotten tab that polls must not keep a 1.5 MiB file forever. Uptime
 *   restarts with the boot, and so does the period.
 *
 * - **A cancelled check leaves the upload `receiving`** with every byte in
 *   place, so it can be checked again. The contract's upload states have no
 *   "cancelled", and `failed` would claim the file was found wanting.
 *
 * Threads and locking: one mutex guards the state. Calls from the HTTP thread
 * (create, get, chunk_accept, chunk_discard, verify_begin, set_in_use,
 * set_active_job, tick) never wait for data I/O; create writes the small
 * metadata file, and an expiring upload is unlinked under the lock. The worker's
 * calls (chunk_commit, verify, delete, the image reader) do their file I/O
 * without the lock and must come from **one** thread at a time - the binding's
 * job worker - which the pending-chunk and verifying flags make safe against the
 * HTTP-side calls. Every function returns 0 or a negative errno.
 */

#ifndef FIRMWARE_STORE_H_
#define FIRMWARE_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <firmware_store/fw_image.h>

#ifdef __cplusplus
extern "C" {
#endif

/** "upload_" and 16 hex digits. */
#define FW_STORE_ID_LEN 23
/** Bytes of the display filename: the schema allows 128 characters of UTF-8. */
#define FW_STORE_FILENAME_MAX 512
/** A job id as job-manager formats it (JOB_ID_MAX_LEN). */
#define FW_STORE_JOB_ID_MAX 31
#define FW_STORE_ERROR_CODE_MAX 31
#define FW_STORE_ERROR_MESSAGE_MAX 127
#define FW_STORE_LAYOUT_ID_MAX 31
#define FW_STORE_HOST_PROTOCOL_MAX 31

/** Upload.state. `verifying` is never stored: after a reboot it is `receiving`. */
enum fw_upload_state {
	FW_UPLOAD_RECEIVING = 0,
	FW_UPLOAD_VERIFYING,
	FW_UPLOAD_READY,
	FW_UPLOAD_FAILED,
};

/** The Upload resource, plus what the binding and the updater need. */
struct fw_upload {
	char id[FW_STORE_ID_LEN + 1];
	char filename[FW_STORE_FILENAME_MAX + 1];
	uint32_t size_bytes;
	/** The next acceptable offset: bytes known to be on storage. */
	uint32_t received_bytes;
	uint8_t sha256[32];
	enum fw_upload_state state;
	/** Set by the binding; "" when none. */
	char active_job_id[FW_STORE_JOB_ID_MAX + 1];
	/** An install holds the file (fw_store_set_in_use()). */
	bool in_use;
	/** A chunk was accepted and is not yet committed. */
	bool chunk_pending;
	/** FirmwareImage, valid when @ref has_image (state ready). */
	bool has_image;
	char version[FW_IMAGE_TEXT_MAX + 1];
	char project_name[FW_IMAGE_TEXT_MAX + 1];
	char idf_version[FW_IMAGE_TEXT_MAX + 1];
	char layout_id[FW_STORE_LAYOUT_ID_MAX + 1];
	char host_protocol[FW_STORE_HOST_PROTOCOL_MAX + 1];
	uint32_t bootloader_bytes;
	uint32_t app_bytes;
	/** ErrorDetail of a failed check: the contract's code and a sentence; "" otherwise. */
	char error_code[FW_STORE_ERROR_CODE_MAX + 1];
	char error_message[FW_STORE_ERROR_MESSAGE_MAX + 1];
};

/**
 * @brief Open the store in @p dir (created if missing) and recover what is there.
 *
 * Safe to call again (the sim tier "reboots" by remounting and calling it). The
 * expiry period starts at @p now_ms.
 *
 * @retval 0       ready; an upload may or may not exist
 * @retval -EINVAL @p dir is NULL or too long
 * @retval -EIO    the directory could not be created or read, or PSA did not start
 */
int fw_store_init(const char *dir, int64_t now_ms);

/**
 * @brief Start a new upload (createUpload).
 *
 * A failed upload is replaced (its files removed first); any other existing one
 * must be deleted by the client.
 *
 * @retval 0       created; @p out describes it (may be NULL)
 * @retval -EAGAIN fw_store_init() has not succeeded
 * @retval -EINVAL empty or too long @p filename, or @p size of 0
 * @retval -EBUSY  an upload exists that is not failed, or an install uses one
 * @retval -EFBIG  @p size is more than CONFIG_FIRMWARE_STORE_MAX_BYTES (413)
 * @retval -ENOSPC the volume lacks @p size plus the reserve (507 storage_full)
 * @retval -EIO    the metadata could not be written
 */
int fw_store_create(const char *filename, uint32_t size, const uint8_t sha256[32], int64_t now_ms,
		    struct fw_upload *out);

/**
 * @brief The upload named @p id (getUpload).
 *
 * @retval 0       @p out filled
 * @retval -ENOENT no such upload, or it expired just now
 */
int fw_store_get(const char *id, int64_t now_ms, struct fw_upload *out);

/**
 * @brief Take a chunk (writeUploadChunk), on the HTTP thread: check it and copy
 *        @p len bytes into the store's staging buffer. Nothing is written yet.
 *
 * Checked in this order, as the mock does:
 *
 * @retval 0          staged; commit it on the worker, or discard it
 * @retval -ENOENT    no such upload (404)
 * @retval -EINVAL    the upload is not `receiving` (409 invalid_state)
 * @retval -EBUSY     a chunk is already pending (409 busy)
 * @retval -ERANGE    @p offset is not received_bytes (409 offset_mismatch)
 * @retval -ENODATA   @p len is 0 (422 validation_failed)
 * @retval -E2BIG     @p len is more than CONFIG_FIRMWARE_STORE_CHUNK_MAX (413)
 * @retval -EOVERFLOW the chunk runs past the declared size (422 validation_failed)
 */
int fw_store_chunk_accept(const char *id, uint32_t offset, const uint8_t *data, size_t len,
			  int64_t now_ms);

/** @brief Drop a staged chunk that will not be committed (e.g. no job could be created). */
void fw_store_chunk_discard(const char *id);

/**
 * @brief Write the staged chunk (worker): data, sync, metadata, sync.
 *
 * The chunk is no longer pending afterwards whatever the outcome; on failure
 * received_bytes has not moved and the client sends the same offset again.
 *
 * @retval 0       on storage; received_bytes advanced
 * @retval -ENOENT no such upload
 * @retval -ENODATA no chunk is pending
 * @retval -ENOSPC the volume lacks what is still missing plus the reserve, or
 *                 filled up during the write
 * @retval -EIO    any other file system failure
 */
int fw_store_chunk_commit(const char *id, int64_t now_ms);

/**
 * @brief Claim the check (verifyUpload) on the HTTP thread: `verifying` from now.
 *
 * @retval 0       claimed; run fw_store_verify() on the worker
 * @retval -ENOENT no such upload
 * @retval -EBUSY  a chunk is pending, or an install uses the upload
 * @retval -EINVAL not all bytes received, or the upload is already `verifying`
 *                 or `ready` (409 invalid_state)
 */
int fw_store_verify_begin(const char *id, int64_t now_ms);

/** Asked between pieces of the check; true stops it. */
typedef bool (*fw_store_cancelled_t)(void *ctx);
/** Bytes checked so far, of @p total. */
typedef void (*fw_store_progress_t)(void *ctx, uint32_t done, uint32_t total);

/**
 * @brief Run the claimed check (worker): read the file through fw_image and
 *        compare its SHA-256 with the declared one.
 *
 * A declared digest that does not match is `invalid_image` and outranks what
 * the structure says - damage in transfer explains both. The outcome is the
 * upload's state (`ready`, or `failed` with the error code and message); read
 * it with fw_store_get(). Either callback may be NULL.
 *
 * @retval 0          the check finished (whichever way)
 * @retval -ENOENT    no such upload, or no check was claimed
 * @retval -ECANCELED stopped by @p cancelled; the upload is `receiving` again
 * @retval -EIO       the file could not be read; the upload is back to its
 *                    state before the claim
 */
int fw_store_verify(const char *id, fw_store_cancelled_t cancelled, fw_store_progress_t progress,
		    void *ctx, int64_t now_ms);

/**
 * @brief Remove the upload and its files (deleteUpload, on the worker).
 *
 * @retval 0       removed
 * @retval -ENOENT no such upload
 * @retval -EBUSY  an install uses it, the image reader has it open, a chunk is
 *                 pending or a check runs (409 busy)
 * @retval -EIO    a file could not be removed; the upload still exists
 */
int fw_store_delete(const char *id);

/**
 * @brief Mark @p id as held by an install, or release it.
 *
 * @retval -ENOENT no such upload
 * @retval -EINVAL holding one that is not `ready`
 */
int fw_store_set_in_use(const char *id, bool in_use);

/** @brief Record the job working on @p id (NULL or "" clears it). -ENOENT or 0. */
int fw_store_set_active_job(const char *id, const char *job_id);

/** @brief Remove an upload that has expired by @p now_ms. For a periodic caller. */
void fw_store_tick(int64_t now_ms);

/**
 * @brief Open a `ready` upload's bytes for the updater (worker).
 *
 * @retval 0       open; @p size is the file's size
 * @retval -ENOENT no such upload
 * @retval -EINVAL it is not `ready`
 * @retval -EBUSY  the reader is already open
 * @retval -EIO    the file could not be opened
 */
int fw_store_image_open(const char *id, uint32_t *size);

/**
 * @brief Read @p len bytes at @p offset of the open image.
 *
 * @return the bytes read (less than @p len only at the end of the file), or
 *         -EBADF when not open, -EIO on a read failure
 */
int fw_store_image_read(uint32_t offset, uint8_t *buf, size_t len);

/** @brief Close the image reader; nothing when it is not open. */
void fw_store_image_close(void);

/** @brief Wire name of a state: "receiving", "verifying", "ready", "failed". */
const char *fw_upload_state_str(enum fw_upload_state state);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_STORE_H_ */
