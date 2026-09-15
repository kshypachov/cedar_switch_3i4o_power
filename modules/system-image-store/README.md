# system-image-store

The staged STM32 application image, written straight into MCUboot's secondary
slot (slot 2), and the check that decides whether MCUboot on this board would
swap it in. Part of the STM32 web update
([report](../../docs/device-development/reports/stm32-update/README.md),
contract section "Обновление STM32" in
[api-contract.md](../../docs/device-development/api-contract.md)).

Headers: [`system_image_store.h`](include/system_image_store/system_image_store.h)
(the store) and [`mcuboot_image.h`](include/system_image_store/mcuboot_image.h)
(the pure streaming check). The design rationale is in the header comments; this
file covers how it fits the system.

## Why a module next to firmware-store, not inside it

The owner decided (2026-09-15) that the bytes of a stm32u585 upload go into
slot 2, not through a file. firmware-store is built around a file (size of the
file vs metadata, truncation, free-space check) and is mutation-tested as it is;
a second sink inside it would touch all of that. This module keeps
firmware-store's API call for call and errno for errno, so the HTTP binding
dispatches by `target` with one mapping table, and firmware-store is untouched.

## Lifecycle

| Call | Thread | What happens |
|---|---|---|
| `sys_img_create` | HTTP | refuses as below, erases the last `trailer_bytes` of the slot, writes `sysimg.meta` |
| `sys_img_chunk_accept` | HTTP | checks, copies into the 16 KiB staging buffer |
| `sys_img_chunk_commit` | worker | erase sectors starting inside the chunk (repair the first one if needed), program, read back, metadata |
| `sys_img_verify_begin` / `sys_img_verify` | HTTP / worker | reads the slot through `mcuboot_image` and the whole-file SHA-256 |
| `sys_img_set_in_use`, `sys_img_image_info` | any | the install holds the upload and reads version and image hash |
| `sys_img_delete`, expiry | worker / any | forget the metadata; the slot is not erased |

States: `receiving` → `verifying` → `ready` | `failed`; a cancelled check is
`receiving` again; a failed upload is replaced by the next create.

## Recovery after a reboot

- Corrupt, short, long or implausible metadata (CRC, version, id prefix, sizes,
  unterminated strings, a stored `verifying`) is removed: no upload.
- `receiving` continues from `received_bytes`. Bytes after it in the slot may be
  a chunk that reached flash without its metadata; resending programs over them
  (identical bytes) or repairs the sector (different bytes).
- `ready` is re-read from the slot: header, TLV info and SHA-256 TLV. If the slot
  no longer holds that image - MCUboot swapped the slots, or anything else wrote
  there - the upload is forgotten. An upload that was installed is therefore gone
  after the swap without the updater deleting it; after a revert (the new image
  back in slot 2) it is `ready` again.
- `failed` is kept as it was, with its error.
- A leftover `sysimg.meta.tmp` is removed.

## errno → contract

| errno | Where | HTTP |
|---|---|---|
| `-ENOENT` | any id lookup | 404 `not_found` |
| `-EAGAIN` | create before init | 503 `service_not_ready` |
| `-EINVAL` | create (arguments) | 422 `validation_failed` |
| `-EINVAL` | chunk (not receiving), verify_begin (incomplete, verifying, ready) | 409 `invalid_state` |
| `-EBUSY` | create (an upload exists), chunk (pending), verify_begin, delete | 409 `busy` |
| `-EFBIG` | create (size > slot − trailer) | 413 `payload_too_large` |
| `-EACCES` | create, chunk (slot locked: running image unconfirmed or swap requested) | 409 `invalid_state` |
| `-ERANGE` | chunk (offset ≠ received_bytes) | 409 `offset_mismatch` |
| `-ENODATA` | chunk (empty) | 422 `validation_failed` |
| `-E2BIG` | chunk (> CHUNK_MAX) | 413 `payload_too_large` |
| `-EOVERFLOW` | chunk (past declared size) | 422 `validation_failed` |
| `-EIO` | create (erase or metadata), commit (flash or metadata), verify (read) | the job fails `internal_error`, retryable |
| `-ECANCELED` | verify cancelled | the job is `cancelled` |

The check's codes are `invalid_image` and `unsupported_target`
(`mcuboot_image_result_code()`), stored in the upload's error.

## What the board provides

`struct sys_img_platform`: `read`/`write`/`erase` over `slot1_partition` (the
application's SPI NOR layout, 4 KiB sectors), `slot_locked()` (true while
`boot_is_img_confirmed()` is false or `mcuboot_swap_type()` is not NONE),
`slot_size` 4 MiB, `sector_size` 4096, `trailer_bytes` 65536 (MCUboot's 64 KiB
sector), and `image`: `header_size` = `CONFIG_ROM_START_OFFSET` (0x400), RAM
ranges for the initial SP (SRAM; PSRAM only if the application's initial stack
could be there - it is not), `exec` = the application's execution window (the
`0x02000000` alias, 4 MiB).

## Kconfig

`SYSTEM_IMAGE_STORE`, `_CHUNK_MAX` (16384), `_EXPIRE_SECONDS` (86400),
`_READ_BUF` (4096), `_SECTOR_MAX` (4096), `_TLV_MAX` (2048), log level.

## Tests

`tests/system_image_store` (native_sim): slot on the sim flash with double writes
(NOR semantics) and fault injection, metadata on LittleFS; corpus in
`tests/fixtures/stm32`.
