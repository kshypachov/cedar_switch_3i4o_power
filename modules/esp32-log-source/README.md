# esp32-log-source

The ESP32-C6's UART output turned into log lines for `log-store`.

Contract: section 7 of `docs/device-development/development-plan.md` and the
Logs row of section 11. The public header carries the rules; this file covers
how the module fits the system.

## What is here and what is not

This module is the **line assembler**: bytes and a clock in, finished lines out
through a callback. It has no thread, no UART and no store. The board's
coprocessor service (`src/services/coprocessor`) owns the rest:

```
USART3 RX interrupt ──▶ ring buffer (bytes only, overflow counted)
                                  │
             coprocessor worker ◀─┘  esp32_log_feed() / esp32_log_poll()
                   │
                   ▼  emit callback
               log_store_append(source=esp32, generation=coprocessor-manager's)
```

That split is what lets every rule be tested in sim, including on the real ROM
stream from board B (`tests/esp32_log_source`, fixture in `tests/fixtures/esp32`).

## Lifecycle

`esp32_log_init()` once. While the console owns the UART, the worker calls
`esp32_log_feed()` with each batch taken from the ring buffer and
`esp32_log_poll()` on its wake-ups, so a partial line is flushed
`CONFIG_ESP32_LOG_SOURCE_IDLE_FLUSH_MS` after its last byte. Before the UART is
handed to someone else (USB bridge, flasher) the manager calls
`esp32_log_flush()`, so the line in progress lands before the `paused` marker.
`esp32_log_note_overflow()` when the ring buffer dropped bytes;
`esp32_log_reset()` to forget a partial line without emitting it.

## Threads and ownership

Not thread-safe: one caller (the coprocessor worker). The callback runs on that
thread, inside feed/poll/flush; the line it gets is valid only during the call.
The assembler holds no pointers into caller memory between calls.

## Rules, and why

| Rule | Why |
|---|---|
| CR, LF and CRLF end a line; empty lines are not records | the ROM prints CRLF, ESP-IDF LF; a blank line is not a message |
| ANSI CSI and two-byte ESC sequences are removed; a CSI past 32 parameter bytes or cut by a control byte is abandoned | ESP-IDF colours its lines; the log screen shows text, and a stray ESC must not eat a line |
| Control bytes (except tab) and malformed UTF-8 become U+FFFD; a run of them is one U+FFFD | the record is JSON text; a C6 in reset or at the wrong baud rate emits bursts of garbage that should not fill a line |
| 512 bytes on a code point boundary, `truncated=true`, the rest of the line dropped | plan section 9; one input line is one record |
| A partial line is flushed after the idle time | a prompt or a line cut by a reset still shows up |
| `L (time) tag: text` gives level and module; V is debug; otherwise level `unknown`, module null | ESP32 records read like STM32 ones; the contract allows `unknown` for UART output |
| `ESP-ROM:` starts a line of its own, wherever it arrives, and flags `rom_banner` | measured on board B: a watchdog reset prints the banner in the middle of the line the ROM was writing (`invalid header: 0xff\xe6ESP-ROM:...`). coprocessor-manager starts a generation on a banner it did not cause |

## Errors

None to report: every byte sequence produces valid lines. What was repaired is
counted in `struct esp32_log_stats` (lines, truncated lines, replacements,
escapes, idle flushes, overflows) for the shell and the report.

## Tests

`tests/esp32_log_source` (sim): line ends, fragments split at every byte,
idle flush, handover flush, ANSI, ESP-IDF prefixes, the UTF-8 table, random
bytes (every line checked for strict UTF-8 and no control characters), the cap,
overflow, the banner inside a line, and the ROM stream of board B as captured
and with its bytes restored. Real C6 output with ESP-IDF levels at 115200 is a
hardware check that waits for firmware on board B's C6 (P6).
