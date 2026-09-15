# system-updater

The STM32 install through MCUboot's secondary slot (slot 2) as one job, and what
the device learns after the swap. Stage "Обновление STM32 через веб"
(`docs/device-development/reports/stm32-update/README.md`), contract
`api-contract.md` "Обновление STM32".

## Lifecycle

```
start()  -> journal stage `preparing`, job accepted (cancellable)
run()    -> preparing   : hold the upload, re-read staged and running image
            [gate]      : job_set_cancellable(false) - a cancel that landed first wins
            requesting  : journal `requesting` (must reach storage), boot_request_upgrade(TEST),
                          swap_pending() must be true
            rebooting   : journal `rebooting`, sleep REBOOT_DELAY_MS, reboot()
--- restart, MCUboot swaps (or not) ---
init()   -> reconcile journal with the running image, arm self-confirmation
tick()   -> confirm when the deadline passed; retry with a doubling delay
```

The job never finishes on this side of the restart: it stays `running` in phase
`rebooting`, and after the restart it is gone (404). The outcome is `last_update`.

## Reconciliation

"new" = the running image is the journal's `to` (version and TLV SHA-256), and -
for a reinstall of the image that ran (`to == from`) - unconfirmed, because MCUboot
leaves a swapped-in test image unconfirmed.

| journal stage | running image | last_update | upload | journal after |
|---|---|---|---|---|
| `preparing` | any | `interrupted` (`boot_changed`) | kept | `none` |
| `requesting` | new | as `rebooting` | consumed | |
| `requesting` | otherwise | `interrupted` (`boot_changed`) | consumed | `none` |
| `rebooting` | new, unconfirmed | `awaiting_confirmation` | consumed | `booted` |
| `rebooting` | new, confirmed | `succeeded` | consumed | `none` |
| `rebooting` | otherwise | `failed` (`internal_error`) | consumed | `none` |
| `booted` | `to`, unconfirmed | `awaiting_confirmation` | - | `booted` |
| `booted` | `to`, confirmed | `succeeded` | - | `none` |
| `booted` | otherwise | `rolled_back` (`boot_changed`) | - | `none` |

`booted` is written at the new image's first start, so a revert before
confirmation reads `rolled_back` and not `failed`. An unreadable running image
leaves the journal untouched until a start that can read it.

"Consumed": once a swap may have happened, slot 2 holds the previous firmware
(which a revert needs), not the upload's bytes; the platform forgets the upload.

## Self-confirmation

Any unconfirmed running image gets a deadline of `CONFIG_SYSTEM_UPDATER_CONFIRM_SECONDS`
(1200, owner's decision) from init. `system_updater_tick(now)` confirms when it
passes; `system_updater_confirm_now()` is the console command. A confirmation
counts when `confirm()` returned 0 and the re-read image is confirmed; otherwise
it is retried after `CONFIRM_RETRY_MS`, doubling up to `CONFIRM_RETRY_MAX_MS`.
While the running image is unconfirmed no install is accepted: slot 2 holds the
image a revert restores.

## Threads and ownership

- `run()` on one worker (the v1 worker on the board); `tick()` from a periodic
  caller; `confirm_now()` from the shell. The confirmation itself is serialised by
  its own mutex.
- Everything else from any thread: a mutex held only for copies; `check()` and
  `get_state()` never touch flash (confirmation and pending swap are cached).
- The module owns no thread and no storage: MCUboot, slot 2, the journal file and
  the restart are the platform's (`struct system_updater_platform`).

## errno → contract

| Function | errno | HTTP |
|---|---|---|
| `check`, `start` | `-EAGAIN` not initialised | 503 service_not_ready |
| | `-EBUSY` an install is active | 409 busy |
| | `-EIO` the running image could not be read | 503 service_not_ready |
| | `-EACCES` running image unconfirmed, or a swap pending | 409 invalid_state |
| `start` | `-EINVAL` bad id | 422 validation_failed |
| | `-ENOENT` no such upload | 404 not_found |
| | `-ENODATA` upload not ready | 409 invalid_state |
| | `-EDOM` downgrade without acknowledgement | 422 validation_failed |
| | `-ENOSPC` journal not saved | 500 internal_error |

Job error codes set by `run()`: `not_found` (the upload is gone), `invalid_state`
(the upload or the running image changed since `start`), `internal_error` (journal,
request, or an unreadable running image).

## Tests

`tests/system_updater` (sim, `cedar.system_updater`) over
`tests/fakes/fake_system_platform.c` and the real job-manager. Notes and mutation
results: `docs/device-development/reports/stm32-update/notes-system-updater.md`.
