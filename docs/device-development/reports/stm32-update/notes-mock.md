# Mock: STM32 update (fork C)

`tools/api-contract/cedar_contract/mock/mcuboot.py` (image check), `mock/system.py`
(install, swap, confirmation), target-aware `mock/firmware.py`; wiring in `state.py`,
`app.py`, `handlers.py`, `server.py`. Rule table: `tools/api-contract/README.md`,
section "The STM32 update stage". Tests: `tests/test_mock_system.py`.

## What the mock does

- `createUpload` with `target="stm32u585"`: limit 4 128 768 bytes (`413` above), no
  LittleFS space check, `409 invalid_state` while the running image is unconfirmed or
  a swap is requested; one upload across both targets (`409 busy`). Without `target`:
  `esp32c6`, unchanged.
- The mock **keeps the bytes of a `stm32u585` upload** and `verifyUpload` parses them;
  `scenario.verify_result` does not apply to STM32. Order: declared SHA-256
  (`invalid_image`), magic (`invalid_image`), header size 0x400 (`unsupported_target`),
  forbidden flags (`invalid_image`), protected TLV/TLV area bounds and file end
  (`invalid_image`), TLV SHA-256 (`invalid_image`), vector table (`unsupported_target`:
  SP in (0x20000000, 0x200C0000] or (0x70000000, 0x70800000], reset odd in
  [0x02000400, 0x02400000)).
- `startSystemUpdate` → job `system_update`: `queued` 300 ms → `preparing` → `requesting`
  → `rebooting`, parked in `rebooting` (never `succeeded`). Cancellable through
  `preparing`. `swap_pending` from the end of `requesting`.
- End of `rebooting` → restart at that moment: new `boot_id`, sessions/jobs gone, **no
  response for `system_swap_ms`** (status 0 in `MockApp.handle`; the HTTP server closes the
  connection without writing anything — `fetch` rejects with a network error). `/__mock/*`
  keeps answering.
- Back: new version running unconfirmed, `confirm_remaining_seconds` from
  `system_confirm_seconds` (counted from the end of the dark period, rounded down),
  `last_update.state=awaiting_confirmation`; the STM32 upload is gone (`404`). On expiry:
  `succeeded`.
- Restart while unconfirmed (`POST /__mock/reboot`) → dark period → `rolled_back`
  (`boot_changed`), old version. Restart before the swap was requested → no dark period,
  `interrupted`, upload stays `ready`.

## Tests and mutations

- `tests/test_mock_system.py` 86 cases; the whole mock suite 464 passed;
  `tests/ci/run-contract-tests.sh`: 5/5 checks (the device serves 35 of 37, not yet
  `getSystemFirmware`, `startSystemUpdate`), 464 + 14 web-assets.
- Existing tests changed only where the contract changed: pinned counts 37 operations,
  31 paths, 51 schemas; `firmware_formats` now `["raw_full_flash","mcuboot_image"]`; the
  coverage walk also reaches the two new operations.
- Mutations (scratchpad copy, runner `mutate_mock.py`): 93 hand-written mutants over
  `mcuboot.py`, `system.py`, `firmware.py`, `app.py`, `jobs.py`, `state.py`,
  `handlers.py`, `server.py`. First pass: 14 survived (the runner also wrongly counted
  failures as build errors, and `test_document_checks` failed in the copy for want of
  `routes.h`). Closed with tests: protected-area bounds and size, missing TLV area, wrong
  TLV magic, cut-off and overrunning TLV entries, no room for vectors, literal addresses
  for the vector window, the park in `rebooting`; one real gap fixed in the mock (a SHA-256
  TLV of the wrong length was skipped, MCUboot refuses it). Second pass: 90 killed,
  3 survived, all equivalent: S24 (`cancelled()` clearing `pending`), S36 (the
  `awaiting_confirmation` guard when confirming), S37 (`settle()` clearing `pending`) — no
  reachable state reads `pending` or a non-awaiting `last_update` at those points.

## For fork D (frontend e2e): knobs and control endpoints

Scenario knobs (`--scenario key=value` at start, `POST /__mock/reset {"scenario":{...}}`,
or `POST /__mock/scenario {...}`):

| Knob | Default | Effect |
|---|---|---|
| `system_version` | `"1.0.0+0"` | running STM32 version at startup/reset |
| `system_confirmed` | `true` | `false`: running image unconfirmed at startup (countdown runs; STM32 upload refused) |
| `system_confirm_seconds` | `1200` | self-confirmation delay after a swap |
| `system_phase_ms` | `1500` | duration of `preparing` and of `requesting` |
| `system_reboot_delay_ms` | `2000` | duration of `rebooting` before the restart |
| `system_swap_ms` | `5000` | dark period (no response at all) |
| `system_swap_result` | `"ok"` | `"rejected"` → `failed`/`invalid_image` after one dark period; `"hangs"` → `failed`/`internal_error` after two |

Control endpoints (outside `/api/v1`):

| Request | Use |
|---|---|
| `POST /__mock/advance {"seconds": N}` | skip the 1200 s confirmation, or the dark period |
| `POST /__mock/reboot` | power cut: performs a pending swap, rolls back an unconfirmed image (each with a dark period) |
| `GET /__mock/state` | `system` (= `SystemFirmware`), `unreachable_for_ms`, `upload`; answers during the dark period |
| `POST /__mock/reset` | fresh device; ends a dark period |

Total time from `startSystemUpdate` to the restart with defaults: 300 + 1500 + 1500 + 2000
= 5300 ms, then 5000 ms dark.

A synthetic image for e2e can be made with `cedar_contract.mock.mcuboot.build_image(body,
version=(1, 0, 1, 0))` (Python) — the check is structural, any small body works.

## Decisions the contract did not make (device should match or the contract should say)

1. `createUpload` `stm32u585` refusal order: `busy` (another upload) → `invalid_state`
   (slot 2 not free) → `413`.
2. `startCoprocessorUpdate` with a `stm32u585` upload: `422 unsupported_target`, right
   after the upload lookup (the contract names this only for `startSystemUpdate`).
3. `SystemFirmware.update` / `features.stm32_update` reasons: `firmware_unconfirmed`,
   `update_running`; available otherwise (a network change or a coprocessor install do
   not make it unavailable, they only refuse the request with `busy`).
4. After a swap (and after MCUboot rejects slot 2) the `stm32u585` upload is removed:
   `getUpload` `404`. After a plain restart without a swap it is kept.
5. `last_update.error` codes: `rolled_back` and `interrupted` → `boot_changed`;
   `failed` rejected → `invalid_image`; `failed` hang → `internal_error`.
6. Downgrade comparison includes the build number (`1.0.1+5` → `1.0.1+4` is a downgrade);
   the same version is allowed without acknowledgement.
7. Check precedence inside verification (above): the vector table is checked last, after
   the TLV SHA-256; a header size mismatch before everything but the magic. A SHA-256
   TLV entry whose length is not 32 is `invalid_image` (as MCUboot's validation does),
   not skipped; a body shorter than the two vector words is `invalid_image`.
8. `confirm_remaining_seconds` counts from when the application starts after the swap,
   rounded down; with the defaults it reads 1199 just after the device is back.
9. A parked job keeps its last step's `cancellable` (jobs.py): `system_update` in
   `rebooting` is `cancellable=false`.
10. An image unconfirmed from startup (`system_confirmed=false`, no install journal)
    restarts its countdown on a reboot — the mock knows no previous image to restore.
