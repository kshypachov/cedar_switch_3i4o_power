# coprocessor-manager

Who owns the ESP32-C6's UART (USART3) and its EN/BOOT lines, and what the log
is told when that changes.

Contract: sections 3, 7 and 8 of `docs/device-development/development-plan.md`,
`CoprocessorStatus` in `docs/device-development/openapi.json`. The public header
carries the full API documentation; this file covers how the module fits the
system.

## Why it exists

Four things want USART3: the ESP32 log source (the C6's console), a USB
passthrough for a person with esptool, the ROM loader library that flashes the
chip (P6), and — until P5 — Zephyr's `uart-bridge0` driver, which claimed the
interrupt at init. Two readers on one UART do not produce an error; they
produce corrupted traffic (section 8). And EN/BOOT, which reset the chip, were
driven from a shell command next to an ESP-Hosted driver that pulses the same
pin at init.

So there is one owner at a time, a switch that makes sure the previous owner
has stopped before the next one starts, and a record in the log of every
handover.

## How the pieces fit

```
  shell / DTR on the CDC ──┐        web-api (GET /coprocessor/status, capabilities)
  P6 updater ──────────────┤                         │ get_status(): never waits
                           ▼                         ▼
                 coprocessor-manager ◀── claim/release ── network-manager
                           │                               (apply, scan)
      platform (src/services/coprocessor on the board, fake_uart in sim)
        ├── uart_attach / uart_detach / uart_rx_activity ─▶ USART3 IRQ handler
        │                                  console: esp32-log-source ring
        │                                  usb_bridge: passthrough to CDC
        ├── c6_reset(download) ────────────────────────────▶ EN (PA8), BOOT (PC0)
        ├── marker(kind, generation, text) ────────────────▶ log-store ESP32 ring
        └── transport_ready ───────────────────────────────▶ ESP-Hosted Wi-Fi device
```

The board's overlay disables `uart-bridge0`, so nothing else installs a handler
on USART3.

## The automaton

```
           ┌────────────── set_mode(usb_bridge) ─────────────┐
           ▼                                                 │
      USB_BRIDGE ──set_mode(console)──▶ CONSOLE ◀──set_mode(console)── FLASHING
                                          │  ▲                            ▲
                                          │  └── set_mode(console) ─┐     │
                                          └── set_mode(flashing) ───┼─────┘
                                                                    │
     attach failed with no owner to put back ──▶ UNAVAILABLE ───────┘
```

- `usb_bridge` ↔ `flashing` directly is refused (`-EBUSY`): one programmer does
  not take the chip from another. `unavailable` is never requested; it is left
  by asking for any other mode.
- A switch: detach the current handler, wait until the interrupt counter stands
  still for two intervals (`RX_QUIET_STEP_MS`, 5 ms), within
  `RX_STOP_TIMEOUT_MS` (100 ms). A switch whose interrupt stops at once takes
  10 ms. If it does not stop, the switch is `-ETIMEDOUT`, the old owner gets its
  handler back and the mode does not change. If the new owner cannot attach,
  the old one is put back (`-EIO`), or the UART is `unavailable`.
- Markers: `paused` when the console is left, written after the console
  stopped; `reset` with a new generation when it comes back, written before its
  handler is attached. So bytes never land on the wrong side of a marker.
- Resets: `reset(download)` pulses EN (with BOOT held for `download`),
  starts a generation, writes a `reset` marker. Refused while `flashing` (the
  flasher drives EN) and while an apply is claimed. A ROM banner later than
  `RESET_WINDOW_MS` (3 s) after the last reset the manager made — or after init,
  since the ESP-Hosted driver resets the chip — is an unexpected restart: a new
  generation and a marker.

## Mutual exclusion

Plan section 3: firmware update, network apply, a manual C6 reset and USB
programming exclude each other; a scan is forbidden while flashing.

| Held | usb_bridge | flashing | reset | apply claim | scan claim |
|---|---|---|---|---|---|
| apply claim | refused | refused | refused | — | allowed |
| scan claim | allowed | refused | allowed | allowed | — |
| usb_bridge | — | refused | allowed | refused | allowed |
| flashing | refused | — | refused | refused | refused |

Both sides are claim-then-check with atomics: network-manager increments its
claim, then reads this module's exclusive bits; a switch or a reset sets its
bit, then reads the claims. At most one of two conflicting operations proceeds,
and no lock is held across the other module — network-manager claims with its
own mutex held.

## Threads

- `set_mode()`, `reset()` and `note_banner()` serialise on a mutex. A switch
  holds it while it sleeps (up to the RX stop timeout); a reset for the pulse.
- `note_banner()` does not wait: while the manager is held, a banner is that
  operation's own. So the coprocessor worker that assembles lines never stalls
  behind a switch.
- `get_status()` copies a snapshot under a spinlock and reads `transport_ready`
  from the platform — an HTTP handler never waits for a switch.
- `claim()`/`release()` take no lock.
- Platform functions (except `transport_ready`) run with the mutex held and must
  not call back into the manager.

No thread of its own, no allocation.

## Errors

| Call | Result |
|---|---|
| `init` | `-EINVAL` missing platform function; `-EIO` console did not attach (runs `unavailable`) |
| `set_mode` | `-EINVAL` unavailable/unknown; `-EBUSY` claim or programmer conflict; `-ETIMEDOUT` RX did not stop; `-EIO` attach failed; `-EAGAIN` before init |
| `reset` | `-EBUSY` flashing or apply claim; the platform's errno (no generation); `-EAGAIN` before init |
| `claim` | `-EBUSY` excluded now; `-EINVAL` unknown |

A refusal is not a failed switch: `last_switch_error` records only
`-ETIMEDOUT`/`-EIO` (and 0 after a successful switch).

## Configuration

| Kconfig | Default | Meaning |
|---|---|---|
| `COPROCESSOR_MANAGER_RX_STOP_TIMEOUT_MS` | 100 | how long the interrupt may take to go quiet |
| `COPROCESSOR_MANAGER_RX_QUIET_STEP_MS` | 5 | interval between looks; a switch takes two |
| `COPROCESSOR_MANAGER_RESET_WINDOW_MS` | 3000 | a banner inside this after a reset is expected |
| `COPROCESSOR_MANAGER_EN_PULSE_MS` | 100 | for the board adapter's EN pulse (`src/plugin_wifi/wifi.c`) |
| `COPROCESSOR_MANAGER_BOOT_HOLD_MS` | 200 | BOOT held after EN release for a download reset |

The module takes `enum log_store_kind` from `log_store.h`: register
`modules/log-store` with it. `CONFIG_COPROCESSOR_MANAGER` does not depend on
`CONFIG_LOG_STORE`, so the sim suite builds without the store's library.

## Testing

`tests/coprocessor_manager` over `tests/fakes/fake_uart.c`: every pair of
modes, markers and generations per transition and their position relative to
the handler, an interrupt that does not stop, a detach that fails, attach
failures with and without an owner to put back, each exclusion in both orders
including a claim arriving mid-switch and mid-reset, counted claims and stray
releases, the reset window, a stand-in flasher that must find the UART free, and
a status read from a second thread while a switch sleeps.

```sh
tests/ci/run-sim-tests.sh -s cedar.coprocessor_manager
```

network-manager's half of the exclusion is tested in `tests/network_manager`.

### Not covered in sim

- **The real UART, EN and BOOT.** A USART3 interrupt that actually stops, the
  pulse timing the C6 needs, switching `console → usb_bridge → console` with a
  host on the CDC, and a reset while the STM32 runs (the W5500 bus fault of P0)
  are the hardware tier's (`docs/device-development/reports/p5`).
- **The flasher.** P6's esp-serial-flasher is replaced here by a stand-in that
  only checks it finds the UART free.
