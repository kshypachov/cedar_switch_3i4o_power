# coprocessor-updater

The UART install of an ESP32-C6 image as one job: the contract's phases, cancellable
until `begin`, a journal that turns an install cut by an STM32 restart into
`interrupted`, and a health check that needs a confirmed firmware version. Public API:
`include/coprocessor_updater/coprocessor_updater.h` (the reasoning is in its header
comment); contract `api-contract.md` "Upload и ESP32 update"; plan sections 3 and 8;
decisions in `docs/device-development/reports/p6/README.md`.

## Lifecycle

```
coprocessor_updater_init(platform)   journal → an active install becomes last_update `interrupted`
binding: coprocessor_updater_check() → job_create(coprocessor_update, cancellable) → coprocessor_updater_start(upload, job)
worker:  coprocessor_updater_run()
```

| Phase | What happens | Failure → `recovery_required` |
|---|---|---|
| `preflight` | image opened; the UART must not be the USB bridge's | no |
| `entering_bootloader` | loader `open` (UART to the flasher, ROM loader, ESP32-C6) | no |
| — | `job_set_cancellable(false)`: a cancel that landed first ends the run here | — |
| `begin` | loader `begin` (the ROM erases) | yes |
| `writing` | image read in `CONFIG_COPROCESSOR_UPDATER_READ_BLOCK` pieces, progress in bytes | yes |
| `verifying` | loader `finish` (MD5) | yes |
| `reconnecting` | image closed, console evidence armed, loader `close` (normal boot, UART back), wait for the ESP-IDF application start on the console | yes |
| `health_check` | platform `transport_restart`: ESP-Hosted again, firmware version | yes |
| `complete` | job `succeeded`, last_update `succeeded` with the version | — |

- **Timeouts**: each phase has a Kconfig limit, checked after every blocking call; a phase
  over its limit fails with `internal_error` "… took longer than N ms". `reconnecting` is
  the exception: running out of time waiting for the console is not a failure — the
  install goes on and records `uart_evidence=false`, because the version is the proof and
  the console loses bytes.
- **Health**: `transport_restart` returning 0 with a version → `succeeded`. `-ENODEV` with a
  version → `succeeded` with `stm32_restart_needed` (the Wi-Fi device was never ready since
  the STM32 started, and Zephyr cannot re-initialise a device whose init failed —
  `reports/p6/notes-design-inputs.md`). Anything else, or no version → `failed`,
  `service_not_ready`, `recovery_required`.
- **Cancel**: before `begin` a cancel ends the run with the job `cancelled`, the loader
  closed if it was open, and last_update unchanged. From `begin` on the job is not
  cancellable and job-manager answers `-EPERM` (409 `invalid_state`).
- **A second install** and **a UART held by the USB bridge** are refused (`-EBUSY`) at
  `check`/`start`, and the bridge is checked again in `preflight`. The coprocessor's state
  (`offline`, `failed`) is not checked: that coprocessor is the one that needs the image.

## The journal

`struct coprocessor_update_journal` is saved by the platform (`/lfs/firmware/update.journal`
on the board, RAM in the sim tier): at `start`, at every phase change, and at the end with
`active=false` and the outcome in `last`. The updater never reads it again after `init`.

- `init` with `active=true`: last_update `interrupted`, `recovery_required` when the phase
  had reached `begin`, error `internal_error` "The STM32 restarted during <phase>; the
  install was not continued", saved at once. Nothing is resumed.
- A journal with another `format` or a phase out of range is treated as no journal; strings
  are terminated on load.
- A save that fails during a run is logged and the install goes on (only `interrupted`
  would be lost); a save that fails at `start` refuses the install (`-EIO`).

## Threads and ownership

- `coprocessor_updater_run()` on one worker; nothing here owns a thread.
- `check`, `start` and `get_state` take a mutex only for copies and never wait for a run;
  the journal is saved outside it.
- The updater does not touch the UART, EN or BOOT: the loader does, through
  coprocessor-manager. It reads the manager's status for the bridge check.
- The image is the platform's (firmware-store on the board): opened in `preflight`, closed
  before the normal boot or on any exit.

## Platform

| Member | Board | Notes |
|---|---|---|
| `loader` | `esp_loader_adapter_updater_loader()` | open cleans up itself on failure; close always hands the UART back |
| `image_open/read/close` | firmware-store, by upload id | `-ENOENT` → `not_found`, other → `invalid_state` |
| `journal_load/save` | a file in `/lfs/firmware` | |
| `boot_evidence_arm`, `boot_evidence` | esp32-log-source's lines after the reset marker | must not block |
| `transport_restart` | the patched `esp_hosted_mcu` restart + version | 0, `-ENODEV` (+version), `-ETIMEDOUT` |
| `now_ms`, `sleep_ms` | `k_uptime_get`, `k_msleep` | |

## Tests

`tests/coprocessor_updater` (native_sim) over `tests/fakes/fake_update_platform.c`, the real
job-manager and the real coprocessor-manager on `tests/fakes/fake_uart.c`: the whole install
with the call order and the journal's phases, a failure and a timeout of every phase with
`recovery_required`, cancel while queued / in preflight / while connecting and its refusal
after `begin`, progress per phase, the journal after a restart in several phases, corrupt
journals, start refusals (second install, USB bridge, bad ids, unsavable journal), health
outcomes (`-ENODEV` with and without a version, no version, timeout). The ESP-Hosted
restart and a real write are the hardware tier's (`reports/p6`).
