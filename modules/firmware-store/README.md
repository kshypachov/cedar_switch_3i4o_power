# firmware-store

The one staged ESP32-C6 image, as a file on LittleFS, and the check that says
whether the chip will boot it.

Contract: section 8 of `docs/device-development/development-plan.md` ("Хранение
образа", "Проверка образа"), "Upload и ESP32 update" in
`docs/device-development/api-contract.md`, the `Upload` and `FirmwareImage`
schemas in `openapi.json`, and the P6 design in
`docs/device-development/reports/p6/README.md`. The public headers carry the API
documentation; this file covers how the module fits the system.

## Why it exists

The browser sends a 1.5 MiB file in 16 KiB chunks over a network that can drop,
to a device that can reboot between two chunks. The contract makes the upload's
offset a promise: `received_bytes` is where a client may resume *after a crash*.
Then the device must decide, before erasing anything on the coprocessor, whether
the file is an image this chip boots with this board's partition layout. Both
jobs need the same file; neither needs the network or the chip.

The owner's decision №1 (P6) made the only image format of the first version the
**full-flash file** `idf.py merge-bin` produces (`raw_full_flash`): bootloader at
`0x0`, partition table at `0x8000`, blank otadata and the application at
`0x10000`, written whole from `0x0`.

## How the pieces fit

```
  HTTP thread (src/web/api/v1)                 job worker (one thread)
  ─────────────────────────────                ───────────────────────────────
  createUpload   fw_store_create()   ─ meta ─▶ upload.meta
  writeUpload…   fw_store_chunk_accept()  ──▶  staging buffer (16 KiB, one chunk)
                                               fw_store_chunk_commit()
                                                 upload.bin: seek, write, sync
                                                 upload.meta: tmp, sync, rename
  verifyUpload   fw_store_verify_begin()       fw_store_verify()
                                                 upload.bin ─4 KiB─▶ fw_image_check
  deleteUpload                                 fw_store_delete()
  getUpload      fw_store_get()                coprocessor-updater
                                                 fw_store_image_open/read/close
```

`fw_image.h` is pure: it takes the file in order, in pieces of any size, and keeps
headers, the 3 KiB table, two descriptions and three SHA-256 contexts. The sim
tier feeds it buffers; the store feeds it the file.

## Lifecycle

`fw_store_init(dir, now)` once at start, after `/lfs` is mounted, before the API
serves. It creates the directory, removes leftovers and recovers the upload (below).
Calling it again re-reads everything, which is how the sim tier "reboots". There
is no teardown.

Upload states: `receiving` → (`verifying`) → `ready` or `failed`. `verifying` is
never written to storage: after a reboot a check in progress is `receiving` with
every byte in place. A cancelled check is `receiving` too. A `failed` upload can
be checked again or replaced by `createUpload`; any other has to be deleted.

## Files and recovery

| File | Holds |
|---|---|
| `<dir>/upload.bin` | the bytes, exactly `received_bytes` long between chunks |
| `<dir>/upload.meta` | id, display filename, declared size and SHA-256, received bytes, state, the check's outcome (image description or error), version, CRC32 |
| `<dir>/upload.meta.tmp` | the next metadata while it is written; renamed over `upload.meta` |

Order of a commit: data, `fs_sync`, metadata to the temporary file, `fs_sync`,
rename. At start:

| Found | Done |
|---|---|
| no metadata | `upload.bin` removed; no upload |
| metadata of the wrong size, magic, version or CRC, or out of range | both files removed; no upload |
| `upload.bin` longer than `received_bytes` | cut back: a chunk reached the data but not the metadata |
| `upload.bin` shorter than `received_bytes` | `received_bytes` = the file's size, written back |
| `ready` or `failed` but the file is not the declared size | `receiving` again, outcome cleared, then the two rules above |
| names starting `upload` other than the two | removed; other names (the updater's journal) left alone |

The client's filename never reaches a path: names on the volume are the three
above, and the directory is the caller's.

## Space, expiry

LittleFS cannot set blocks aside, so space is **checked**: `create` wants the
declared size rounded up to the volume's allocation unit plus
`CONFIG_FIRMWARE_STORE_RESERVE_KIB` free, and each commit wants what is still
missing plus the reserve. `storage_full` therefore comes before the first byte;
if another writer (settings) fills the volume later, the chunk's job fails with
`storage_full` and the offset does not move. This is weaker than a reservation,
and it is what the file system allows.

An upload untouched for `CONFIG_FIRMWARE_STORE_EXPIRE_SECONDS` of uptime (a day)
is removed the next time anyone asks the store about it, or on `fw_store_tick()`.
Creating, a chunk accepted or committed and a check count as activity; reading
does not, so a forgotten tab polling `getUpload` does not keep the file. An
install holding it, a pending chunk, a check or an open reader postpone expiry.
Uptime starts again with each boot, and so does the period.

## Threads and ownership

One `k_mutex` guards the state. HTTP-thread calls (`create`, `get`,
`chunk_accept`, `chunk_discard`, `verify_begin`, `set_in_use`, `set_active_job`,
`tick`) never wait for data I/O: `create` writes the small metadata file and
expiry unlinks under the lock, the rest is RAM. Worker calls (`chunk_commit`,
`verify`, `delete`, the image reader) do their I/O **without** the lock and must be
serialised by the caller - the binding's one job worker. The staging buffer and
the file are safe from the HTTP side because every HTTP-side mutation refuses
while a chunk is pending or a check runs (`-EBUSY`/`-EINVAL`).

`fw_store_set_in_use()` is the updater's hold: while set, the upload cannot be
deleted, replaced, re-checked or expired. The image reader is a second, separate
hold (open file).

Memory: `.bss` of `libfirmware_store.a` - the 16 KiB staging buffer, a 4 KiB read
buffer, one `fw_image_check` (≈4 KiB) and the metadata copy (≈1 KiB). Nothing runs
in an ISR or feeds DMA (`/lfs` is SPI NOR without DMA), so the firmware may place
it in PSRAM.

## Errors

| Call | errno | Contract |
|---|---|---|
| `create` | `-EINVAL` | 422 `validation_failed` (the schema catches most of it first) |
| | `-EBUSY` | 409 `busy` |
| | `-EFBIG` | 413 `payload_too_large` |
| | `-ENOSPC` | 507 `storage_full` |
| | `-EIO`, `-EAGAIN` | 500 / 503 |
| `get` | `-ENOENT` | 404 `not_found` |
| `chunk_accept` | `-ENOENT` | 404 |
| | `-EINVAL` | 409 `invalid_state` |
| | `-EBUSY` | 409 `busy` |
| | `-ERANGE` | 409 `offset_mismatch` |
| | `-ENODATA`, `-EOVERFLOW` | 422 `validation_failed` |
| | `-E2BIG` | 413 `payload_too_large` |
| `chunk_commit` (job) | `-ENOSPC` / `-EIO` | job `failed`: `storage_full` / `internal_error` |
| `verify_begin` | `-EBUSY` / `-EINVAL` | 409 `busy` / `invalid_state` |
| `verify` (job) | 0 | job ends; outcome is the upload's state and error |
| | `-ECANCELED` | job `cancelled`; upload `receiving` |
| | `-EIO` | job `failed`, `internal_error` |
| `delete` (job) | `-EBUSY` | 409 `busy` (check it before creating the job) |

The check's outcomes: a declared SHA-256 that does not match, or anything
structural, is `invalid_image`; a foreign chip id `unsupported_target`; a
partition layout other than `cedar-c6-ota-4m-2x1792k` `incompatible_firmware`.
The declared digest outranks structure: damage in transfer explains both. The
messages are English sentences meant for the `ErrorDetail.message`.

## What the check does not decide

Which CP build the file is (Wi-Fi or OpenThread): identical headers, owner's
decision №4 deferred. `host_protocol` is the profile's value, not read from the
image. No signature: the digests find damage, not authors.

## Tests

`tests/firmware_store` (native_sim, LittleFS on a 1 MiB sim-flash partition, SHA-256
through PSA): MD5 vectors, the check on the synthetic corpus in
`tests/fixtures/esp32/firmware` fed in pieces of 1 byte to 32 KiB, single-byte
faults at each rule, and the store's lifecycle, errno contract, recovery,
leftovers, space and expiry. The corpus is generated and independently judged by
`tests/fixtures/esp32/firmware/fwimage.py`, which also checks a real merged build
on the host (`gen_firmware_fixtures.py --merge`).
