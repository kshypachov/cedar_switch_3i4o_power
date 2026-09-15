# Coprocessor service (board)

The board side of the ESP32-C6's UART and reset lines: coprocessor-manager's
platform, the USB CDC passthrough, and the ESP32 log source running on a worker.
The decisions live in the modules; this file covers how they meet the hardware.

| Module | What it decides |
|---|---|
| `modules/coprocessor-manager` | who owns USART3 and EN/BOOT, markers, generation, exclusion with the network |
| `modules/esp32-log-source` | bytes → lines: UTF-8, ANSI, 512 bytes, idle flush, ESP-IDF prefix, ROM banner |
| `modules/log-store` | the rings the lines go to |

## Hardware

| Resource | DT | Notes |
|---|---|---|
| C6 UART | `&usart3` (PC10/PC11), 115200 | the board's `uart-bridge0` is disabled in the app overlay, so nothing else installs a handler at init |
| USB end of the bridge | `cdc_acm_uart0` ("Zephyr USB CDC-ACM uart0") | `cdc_acm_esp` ("ESP32 Bridge") is not used; see "Which CDC" |
| EN | `wifi_reset` (PA8, active low) | also the ESP-Hosted driver's `reset-gpios`, which it pulses only in its init |
| BOOT strap | `wifi_boot` (PC0, active low) | held while EN is released: ROM download mode |

## Ownership

- **USART3's interrupt**: installed only by `op_attach()` and removed only by
  `op_detach()`, both called by coprocessor-manager with its mutex held. Three
  handlers: `console_isr` (bytes into `console_rx`), `bridge_uart_isr` (bytes
  to `to_host`, `to_chip` out), none for `flashing`.
- **The CDC's interrupt**: installed once at start. Outside the bridge, what the
  host sends is read and dropped (`host_bytes_discarded`), so the host's end
  never backs up.
- **EN/BOOT**: `op_reset()`, called by the manager. `wifi_ctrl reset|init`
  (src/plugin_wifi) go through `coprocessor_manager_reset()`.
- **The line assembler**: `asm_lock`. The worker holds it while feeding bytes;
  `op_marker()` holds it to drain the console ring, flush the partial line and
  write the marker. Lock order is manager → `asm_lock`, never the other way: a
  ROM banner seen while feeding only sets `banner_seen`, and the worker calls
  `coprocessor_manager_note_banner()` after releasing `asm_lock`.

## Threads

| Context | Does |
|---|---|
| USART3 / CDC interrupts | copy bytes between the UART FIFO and the rings; count drops; nothing else |
| worker `coprocessor` (preemptive 12, 2 KiB stack, SRAM) | every 20 ms (sooner when the console ring is half full): drain `console_rx` into the assembler, idle flush, ROM banner → manager; every 100 ms: follow DTR and the CDC's baud rate |
| caller of `coproc mode` / `wifi_ctrl` / DTR | the manager's switch, including `op_marker()` |

Everything an interrupt touches is in this library, which is not relocated to
PSRAM (root CMakeLists.txt, `psram_sections.ld`).

## Turning the USB bridge on

The owner asked for whichever way switches fastest (reports/p5). Two ways, no
API operation (the contract only reads `uart_mode`):

- **DTR** on `cdc_acm_uart0`: a host that opens the port (DTR high) gets the
  bridge; DTR low for a second after that hands the UART back to the console.
  Only a bridge DTR started is ended by DTR. `coproc dtr off` stops following it.
- **Shell**: `coproc mode bridge|console|flashing`, which also prints how long
  the switch took.

In the bridge the USART3 baud rate follows the CDC's line coding (esptool
raises it after syncing); the console's configuration is restored when the
console takes the UART back.

## The passthrough's transmit wake-ups

`cdc_acm_irq_tx_enable()` (usbd_cdc_acm.c) queues a work item on the USB thread every
time it is called while its FIFO has room, and USART3 interrupts once per byte, so
enabling the CDC's TX per received byte meant ~11.5 thousand queued works a second
(`usbd` took ~28 % of the busy threads' cycles on board B, reports/p5 hw §26). Each
direction now has an idle flag:

1. The side that sends (`fill_from()`) finds its ring empty: TX off, flag set, look at
   the ring again and, if something arrived meanwhile and it takes the flag back
   (`atomic_cas`), TX on.
2. The side that receives queues bytes and enables the peer's TX only if it takes the
   flag (`wake_tx()`).

Every interleaving leaves either TX on or the flag set with an empty ring, so no byte
waits with TX off. `coproc status` counts the host-side wake-ups. This path is board
code without a sim test; it is checked by the bench scenarios `uart` and `bridge-load`.

## Failure modes

| Situation | What happens |
|---|---|
| The USB host does not read the CDC | `to_host` fills, further C6 bytes are dropped and counted (`dropped_to_host`); USART3 keeps receiving, nothing blocks |
| The C6 floods the console | `console_rx` overflows, `console_overflows` → a U+FFFD and `truncated` on the damaged line, `LOG_STORE_LOSS_UART` |
| RX does not go quiet on a switch | the manager's `-ETIMEDOUT`; mode unchanged, old handler back |
| log-store refuses an append | `LOG_STORE_LOSS_BACKEND` |

## Which CDC

The board DT declares two CDC ACM functions with the same USB serial
(3543501200210047 on board B): `cdc_acm_uart0` and `cdc_acm_esp` ("ESP32
Bridge"). Only `cdc_acm_uart0` was ever a bridge peer (`uart-bridge0`) and it
is the one the bench MCUboot uses for recovery, so the passthrough stays on it.
`cdc_acm_esp` is a DT node nothing opens. On a host the two are told apart by
interface number, not serial - the method is recorded in reports/p5/hw.

## Shell

| Command | |
|---|---|
| `coproc status` | mode, generation, transport, switch and reset counters, bytes and drops, assembler stats |
| `coproc mode console\|bridge\|flashing` | switch, with the time it took |
| `coproc reset [download]` | EN pulse; `download` holds BOOT |
| `coproc dtr on\|off` | follow DTR or not |
| `coproc logs` | log-store rings, losses, lock hold times |
| `coproc upload drop` | debug: removes the staged upload's files in /lfs/firmware and reopens firmware-store, for an upload whose id nobody holds any more; refused during an install (P6) |
| `coproc loader` | debug, read-only for the C6: esp-loader-adapter session open (ROM loader, connect, ESP32-C6 check) and close (normal boot, EN/BOOT idle, console back); refused during an install (P6) |
| `coproc burst <n> [len]` | debug: n `LOG_INF` messages from the shell thread, for the STM32 burst on the bench (`log enable dbg` has nothing to enable: modules are compiled at the default level) |
| `wifi_ctrl reset`, `wifi_ctrl init` | kept debug commands, through the manager (`init` = ROM download mode) |
