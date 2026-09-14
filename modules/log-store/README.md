# log-store

The bounded record rings behind the log screen, the log API and the export.

Contract: section 7 of `docs/device-development/development-plan.md`, the "Логи"
section of `docs/device-development/api-contract.md`, `LogRecord`/`LogPage` in
`openapi.json`. The public header carries the API documentation; this file covers
how the module fits the system.

## Why it exists

Two producers write logs the browser wants to see — the Zephyr log backend
(`zephyr-log-source`) and the C6's UART (`esp32-log-source`) — and any number of
browsers read them, some of them slowly. The plan's rule is that a stuck browser
must never slow down logging. So the store is a fixed amount of memory (512 KiB of
PSRAM in the starting budget), a reader that falls behind loses the oldest records
and is told so (`gap`), and nothing a reader does makes a producer wait longer than
one record copy.

## How the pieces fit

```
  Zephyr log thread ──▶ zephyr-log-source ─┐
                                           ├─ log_store_append() ──▶ ring STM32
  coprocessor worker ─▶ esp32-log-source ──┘                    └─▶ ring ESP32
                                                   (global seq under one mutex)

  HTTP thread (src/web/api/v1/logs.c)
     log_store_reader_tail() / log_store_reader_at(cursor)
     log_store_read() ... one record per lock hold, filter and JSON outside
     log_store_cursor_encode()
```

## Lifecycle

`log_store_init()` once, before the log thread starts delivering messages (the
firmware's backend reports "not ready" until then, so messages written at boot
wait in the log core's deferred buffer rather than being lost). There is no
teardown: the rings live for the boot. `log_store_panic()` is final for the boot.

## Records and memory

A record is a 28-byte header (length, level, flags, lengths, generation, seq,
uptime), the module name (≤ 64 bytes), the text (≤ 512 bytes), padding to a
multiple of four and a 4-byte trailer holding the length again, so the ring can be
walked from the head backwards. The smallest record is 32 bytes, the largest 608.

Measured in the sim suite (`test_records_per_ring_for_typical_lines`, module
`net_dhcpv4`, texts of 23–62 bytes, 36 on average): **80 bytes per record** (51
records in a 4 KiB ring), so about 3 276 in a 256 KiB ring. A typical STM32 line
(short module, 30–60 byte message) is 72–104 bytes by the layout. The C6 ROM loop
`invalid header: 0xffffffff` with no module is 60 bytes — about 4 400 lines per
ring, which at the ROM's rate
(28.5 KB in the link check) is seconds of history; that is what a separate ring
per source is for.

Placement: the rings and all state are ordinary `.bss` of `liblog_store.a`; the
firmware lists that archive in `src/helpers/psram_sections.ld` to put them in
PSRAM. Nothing here runs in an ISR, so nothing needs SRAM.

## Threads and the lock

One `k_mutex` guards both rings, the sequence counter and the timing maxima.

- **Append** takes it for: evicting old records (reading one length per evicted
  record), assigning `seq`, copying ≤ 608 bytes. Commit order is therefore
  sequence order across both sources, which is what lets `source=all` merge the
  rings by `seq` without ever emitting 10 before 9.
- **Read** takes it for: clamping the reader's positions to the tails (setting
  `gap`), peeking at the next header of each selected ring, copying the one
  record with the lowest `seq`. Filtering, JSON and sending happen after unlock.
- **Tail** (no cursor) does the same backwards, one record per hold.
- Loss counters have a spinlock of their own: a panicking backend counts from a
  fault context where the mutex must not be touched.

`log_store_get_timing()` records the longest append hold, the longest wait of an
append for the lock and the longest read hold, measured with `k_cycle_get_32()`;
the board figures are in `docs/device-development/reports/p5`.

**Why a mutex, not a spinlock.** A spinlock masks interrupts for the copy. The
USART3 receive interrupt that feeds the C6 source must not wait on a web client
reading records, and 608 bytes copied from PSRAM is long compared to a byte at
115200 baud.

**Why not a lock-free reader (seqcount).** Considered in the design: readers copy
without the lock and re-check the tail afterwards. It is correct on a single core,
but its whole point is the interleaving of a copy with an eviction, which the sim
tier cannot produce on demand and mutation testing cannot tell apart from a
broken check. A one-record critical section gives the same bound to the producer
("waits at most one copy") with a property that can be tested and measured.

The one-record-per-hold rule is a property of the code, not of a test: every
`k_mutex_lock()` in `log_store.c` is followed by at most one `copy_record()`.

## Readers and cursors

Positions are absolute byte offsets (bytes ever written to that ring), aligned to
four. A position behind the ring's tail means the records there were overwritten:
the reader moves to the tail and `gap` is set. A position past the head is not
one this store ever produced.

A read is bounded by the scan budget (records looked at, matching or not); when
it runs out, the reader's position continues the scan, so a filter that matches
nothing still answers promptly and never makes a client rescan what it passed.
`reader.scanned` accumulates across reads so a caller can spend one budget on a
whole page.

The cursor is 29 bytes as 39 characters of unpadded base64url: version, a 32-bit
tag of the boot id, a 32-bit digest of the filter, both positions, and a FNV-1a
checksum with a fixed salt (integrity, not secrecy — the API is behind a session).
Decoding checks, in order: shape and checksum, filter digest (`-EINVAL`), boot tag
(`-ESTALE`), positions past a head or not on a 4-byte boundary (`-EINVAL`). Only
the stale-boot case is not a client error: the API answers it like a request with
no cursor, with the new `boot_id` and `gap=true`.

A filter with `sources == 0` matches nothing; its reader stands at both heads, so a
cursor from it continues "from now" (the binding uses it for a module name longer
than any record can hold).

## Errors and losses

| Situation | What happens |
|---|---|
| Invalid source, level or kind; NULL text with a length | `log_store_append()` → `-EINVAL`, no sequence number taken |
| Before init, after panic | `-EAGAIN`; the producer counts `LOG_STORE_LOSS_BACKEND` |
| Text longer than 512 bytes | cut on a code point boundary, `truncated=true` |
| Module longer than 64 bytes | cut on a code point boundary (not a truncation of the message) |
| Ring full | oldest records evicted, `overwritten` counted; readers see `gap` |
| Log core dropped messages, UART overflowed | producer calls `log_store_count_loss()`; `log_store_dropped()` is the API's `dropped_count` |
| A record length that fails sanity (memory corruption) | the ring is started over (append) or treated as ended (read); never walked |

The module never logs.

## Tests

`tests/log_store` (sim tier), run at 4 KiB rings (`cedar.log_store`) and at the
firmware's 256 KiB (`cedar.log_store.production_sizes`): wrap and overwrite
counts, independent rings, gap for a slow reader and for a cursor behind the tail,
tail with and without filters and across both rings, scan budget continuation
through a cursor, `upper_seq` snapshot, cursor round trip, filter/boot binding,
every single-character corruption, positions past the head or between records,
filters (min_level keeps null and unknown, module exact, contains ASCII
case-insensitive), `sources == 0`, text and module cuts, `log_store_utf8_cut`
edges, a producer thread appending while a slow reader reads (no record twice, out
of order, or skipped without a gap), panic, loss counters, stats and wire names.
