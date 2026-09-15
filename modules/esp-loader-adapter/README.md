# esp-loader-adapter

The thin layer between the coprocessor updater and
[espressif/esp-serial-flasher](../../../modules/lib/esp-serial-flasher) v2.0.0: one
flashing session over the ESP32-C6's UART with a cleanup that always runs. Public API:
`include/esp_loader_adapter/esp_loader_adapter.h` (the reasoning is in its header
comment); plan section 3 (module row) and section 8 ("Запись по UART"); decisions in
`docs/device-development/reports/p6/README.md`.

## Lifecycle

```
esp_loader_adapter_init(lib)
esp_loader_adapter_open()      coprocessor_manager_set_mode(flashing) → port_init → connect → target == ESP32-C6
                               [→ change_rate, only with CONFIG_ESP_LOADER_ADAPTER_HIGH_BAUD]
esp_loader_adapter_begin(n)    flash_start(0x0, n rounded up to 4) — the ROM erases under the write
esp_loader_adapter_write(...)  whole blocks of CONFIG_ESP_LOADER_ADAPTER_BLOCK_SIZE
esp_loader_adapter_finish()    last block padded with 0xFF → flash_finish (MD5)
esp_loader_adapter_close()     reset_target → port_deinit → lines_idle → console_restore → set_mode(console)
```

A failure of any library call in `open`, `begin`, `write` or `finish` runs the whole
cleanup before the call returns; `close()` is then a no-op. After a fully successful
write the caller closes. Every cleanup step runs whatever failed before it, and the first
failure is reported.

| Step failed | What the cleanup does |
|---|---|
| the manager refused the UART (`-EBUSY`: USB bridge, apply or scan claim) | nothing: the UART, EN and BOOT belong to someone else |
| the UART did not go quiet (`-ETIMEDOUT`) | nothing: the console kept the UART |
| the switch failed otherwise (`-EIO`) | lines idle, console configuration, console (the manager may have left `unavailable`) |
| `port_init` | lines idle, console configuration, console — no library call on a port that did not come up |
| connect, target, rate, begin, a block, finish | normal boot, deinit, lines idle, console configuration, console |

## Why the port lives only inside a session

`esp_loader_init_serial()` installs tty's interrupt handler on USART3 with receive
enabled. When tty's 512-byte receive ring overflows, `uart_rx_handle()` writes a `~` for
every dropped byte — from the ISR, with `K_FOREVER`, into a transmit ring that nothing
drains unless a library call is running (and the `~` also goes out to the C6). An
unflashed C6's ROM restarts every 0.65 s and prints each time, so a port left initialised
with no reader blocks that ISR forever: board B hung at boot this way when the library's
device init ran at POST_KERNEL (`reports/p6/README.md`, `hw/logs/02`). Therefore:

- the application's `espressif,esp-loader` node is `zephyr,deferred-init`: the kernel never
  initialises the device, `device_is_ready()` is always false and is never asked, and
  nothing calls `device_init()`;
- `port_init` runs only in `open()`, after coprocessor-manager has detached the console
  (mode `flashing`), immediately before connect;
- `port_deinit` (UART interrupts off) follows the last library call of every path, and a
  successful session lasts only as long as the updater's phases, each bounded by its
  timeout. In the ROM loader the C6 prints nothing, so the short gaps between the
  updater's calls do not fill the ring.

## What it does not do

No address from outside (always `0x0`, the verified size), no stub, no deflate, no
erase outside the write, no eFuse, no secure download mode. Retrying a block is the
library's (`CONFIG_SERIAL_FLASHER_WRITE_BLOCK_RETRIES`); a block that still fails ends
the session — the ROM's sequence numbers are broken.

## Threads and ownership

- One session at a time, on the updater's worker; calls block as long as the library
  does and must not run in an ISR or under another module's lock.
- **USART3's interrupt**: coprocessor-manager's console before `open()` and after the
  session; tty's handler (the library's) in between. The manager's mode is `flashing`
  for exactly that window, so no log or bridge handler shares the UART.
- **EN/BOOT**: driven by the library during the session; `esp_loader_deinit()` leaves them
  `GPIO_DISCONNECTED`, so the board's `lines_idle` makes them inactive outputs right away.
- **USART3's configuration**: the library changes the baud rate only through
  `change_transmission_rate`; `console_restore` puts the console's configuration back
  before the console attaches, whether or not the rate changed.

## Files

| File | Built with | What |
|---|---|---|
| `lib/esp_loader_adapter.c` | `CONFIG_ESP_LOADER_ADAPTER` | the session core; uses only the table and coprocessor-manager |
| `lib/esp_loader_adapter_zephyr.c` | `CONFIG_ESP_LOADER_ADAPTER_ZEPHYR` (esp-serial-flasher and a chosen `zephyr,esp-loader`) | `esp_loader_adapter_zephyr_fill()`: the `esp_loader_*` calls on the device's static data — `esp_loader_from_device()`, `esp_loader_connect_args_from_device()`, and the port, which `zephyr_port.h` does not expose: it is the first member of the device data (`struct esp_loader_dev_data { zephyr_port_t interface; esp_loader_t loader; }` in `port/zephyr_port.c`). Being the library's private layout, it is checked at run time against `esp_loader_config_from_device()`; `port_init` fails rather than guess |
| `lib/esp_loader_adapter_updater.c` | `CONFIG_ESP_LOADER_ADAPTER_UPDATER_OPS` | `esp_loader_adapter_updater_loader()`: the session as `struct coprocessor_updater_loader` |

The board supplies `lines_idle`, `console_restore` and `ctx` and calls
`esp_loader_adapter_zephyr_fill()` for the rest.

## Errors

| Situation | errno | ErrorDetail code | retryable |
|---|---|---|---|
| UART in use (bridge, apply, scan), session already open | `-EBUSY` | `busy` | yes |
| UART did not go quiet, switch failed, port init | `-EIO` | `internal_error` | yes |
| ROM loader silent (`ESP_LOADER_ERROR_TIMEOUT` on connect) | `-EIO` | `service_not_ready` | yes |
| ROM loader answered wrongly on connect | `-EIO` | `internal_error` | yes |
| target is not an ESP32-C6 | `-ENOTSUP` | `unsupported_target` | no |
| higher rate refused, begin refused, block refused, last block refused | `-EIO` | `internal_error` | yes |
| MD5 mismatch (`ESP_LOADER_ERROR_INVALID_MD5`) | `-EIO` | `internal_error`, message names MD5 | yes |
| calls out of order, size 0, past or short of the size | `-EPERM` / `-EINVAL` | `internal_error` | no |

`err.cause` carries the library's code (or the manager's errno) for the log.

## Tests

`tests/esp_loader_adapter` (native_sim, two configurations: default and
`CONFIG_ESP_LOADER_ADAPTER_HIGH_BAUD=460800`) over `tests/fakes/fake_esp_loader.c` and the
real coprocessor-manager on `tests/fakes/fake_uart.c`: every open refusal and failure with
its cleanup order and the UART owner at each step (port init only in `flashing`), that no
failure point returns with the port initialised, padding and block framing, order rules,
close with failing steps, the new log generation, a second session, and the updater ops.
The real table is compile-checked for the board with a `zephyr,deferred-init` node; a real
write is the hardware tier's (`reports/p6`).
