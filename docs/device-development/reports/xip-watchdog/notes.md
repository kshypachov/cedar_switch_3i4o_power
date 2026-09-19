# IWDG reset during STM32 firmware upload (XIP flash write)

Task from the owner, 2026-09-18: find out why the STM32 is reset by the independent
watchdog when the firmware writes the OCTOSPI NOR it executes from
(`zephyr/drivers/flash/flash_stm32_ospi_xip.c`, `CONFIG_FLASH_STM32_OSPI_XIP`). Owner:
it happens mostly when flashing over the network. Method: hypothesis → code change →
test on the board; a change that does not help is reverted.

Bench: board A (ST-Link `002F002B3233510739363634`, console `/dev/cu.usbmodem1403`),
Ethernet 192.168.88.21, Wi-Fi 192.168.88.19 (DHCP; the .14 of older notes is stale).

## Baseline (image built from the unmodified tree, 22:41)

| Test | Result |
|---|---|
| `sysupd selfcopy` (erase + write 1.4 MB of slot 2 from the shell) | ok, 10.06 s; kernel time matched wall time |
| API upload over Ethernet, 16 KiB chunks, 4 runs | 4/4 ok, 210–221 s each, no reset |
| Full web cycle over Ethernet (upload → verify → install → swap → back → `sysupd confirm`), baseline image | 1/1 ok: upload 234 s, back 72 s after install |
| Same, installing the test image (baseline + `xipbus`) | 1/1 ok: upload 232 s, back 73 s |

## Observation trap and a tool artefact

- `STM32_Programmer_CLI … mode=HOTPLUG` on the running board **clears the NVIC**
  (ISER = 0, ISPR = all ones, IPR = 0). SysTick is not an NVIC line, so the kernel
  keeps ticking and `app_iwdg` keeps feeding the watchdog, but UART shell, W5500 and
  ESP-Hosted SPI stop (their threads pend on ISRs that never come). Reproduced twice,
  once on a fresh boot. The board was unreachable 22:43–22:49 because of this, not
  because of the firmware. Observe with pyocd (`target_override="cortex_m"`,
  `connect_mode="attach"`), which does not touch the NVIC.
- Trap for the real hang: ping every 0.5 s; on 3 losses pyocd halts the core, sets
  `DBGMCU_APB1FZR1.DBG_IWDG_STOP`, dumps core/fault/NVIC/OCTOSPI/DCACHE registers,
  the XIP guard statistics and every thread's blocked call chain.

## Where the network path differs from `selfcopy`

The upload writes slot 2 from the `web_v1_jobs` work queue: stack
`v1_worker_stack` at 0x70029798 (PSRAM) and the source `staging` at 0x7015c42c
(PSRAM, `libsystem_image_store.a` .bss). `selfcopy` runs on the shell thread (stack
0x20042130, SRAM) with `copy_buf` at 0x200228c4 (SRAM).

The PSRAM is not an independent device: the board DTS puts OCTOSPI2 (PSRAM) on
OCTOSPIM port 1 **together with** OCTOSPI1 (NOR), sharing CLK and IO0..3, with only
its own nCS (PA0) — OCTOSPIM multiplexed mode. The bus goes to the other controller
only when the current one releases nCS.

## Hypothesis 1: PSRAM access inside the page-program data phase deadlocks the bus

In `xip_pp()` the page program starts on the write of AR; from then on OCTOSPI1
keeps nCS low until the CPU has pushed all `len` bytes into its FIFO. The loop
(disassembly of the baseline image, 0x20007040–0x2000705e) does, per byte:

- `ldrb r1, [r9, r4]` — read `data[i]`: the bounce buffer on the caller's stack,
  i.e. PSRAM for the web path;
- `bl xip_wait_sr` — which pushes and pops 8 registers on the caller's stack (PSRAM).

If one of these misses DCACHE1, the CPU waits for OCTOSPI2, OCTOSPI2 waits for
OCTOSPI1 to release nCS, OCTOSPI1 waits for the CPU to feed its FIFO: the core stalls
on a bus transaction that never completes, with interrupts masked, and the IWDG resets
it ~20 s later. Hits in DCACHE1 make the stall rare and random, which fits "sometimes
during network flashing, never in `selfcopy`".

The comment in `src/services/system/system_service.c` (`stack_in_sram()`, confirmation
refused on a PSRAM stack) already treats the PSRAM stack as unsafe for this path; the
upload path does not have that guard.

### Result: refuted

- `xipbus sram 64 flush` 64/64 in 273 ms; `xipbus psram 64` and `xipbus psram 64 flush`
  64/64 in 418/442 ms (src/diagnostic/xip_bus_test.c). The `flush` variant alone would
  prove nothing — the driver `memcpy`s the source into its stack bounce buffer after
  the flush and before the guard, refilling the cache.
- What decides it: **DCACHE1 is off at run time** (`DCACHE1_CR` = 0x300, EN = 0; nothing
  in the app, the SoC code or `drivers/cache/cache_stm32.c` enables it). Every PSRAM
  access goes to the bus. The web path therefore reads the bounce buffer and pushes
  `xip_wait_sr`'s frame over OCTOSPI2 on every byte of every page program, and
  ~5.8 MB of web uploads (~23 000 pages) plus the `xipbus psram` runs completed.
- `OCTOSPIM_CR` = 0x1 (MUXEN = 1, REQ2ACK_TIME = 0), `P1CR` = 0x02010101,
  `P2CR` = 0x06040322 read on the board: the multiplexing is real, and the OCTOSPIM
  resolves the PSRAM request during a page program without deadlock.
- Side fact: `xip_stm32_ospi_cache_invalidate()`'s DCACHE branch never runs for the
  same reason.

No driver change was made for hypothesis 1.

## Series over Wi-Fi with the update page's polling (2026-09-18/19)

Test image = baseline + `xipbus`, flashed and installed through the web API each run.
A background poller imitates the page's scheduler (one request at a time):
`system/firmware` 2 s, `system/status` 10 s, `capabilities` 30 s. A first poller that
kept 7 keep-alive connections was an artefact: the server has 4 client slots, uploads
got `Connection refused`/timeouts. With the logs page's `/logs/records` every 1 s
(0.5–1.1 s per answer) uploads ran ~10× slower (0.7 KiB/s) but completed.

| Run | Transport | Result |
|---|---|---|
| series 3 run 1 | Ethernet, heavy poller | ok: upload 920 s, swap, back 69 s, confirmed |
| series 4 (6 runs, 00:12–01:12) | Wi-Fi, update-page poller, resume on error | 3 full cycles ok (726–774 s); 3 runs broken by network stalls (two `verify` timeouts, one after a trap halt); no reboot during any upload |

Total by 01:12: 8 full web cycles (upload → verify → install → swap → confirm) and
4 API uploads, **no IWDG reset**.

### Traps that turned out to be false

A ping-based trap fired three times (00:21:56, 00:46:06, 00:47:23). Each time the IWDG
was being fed (`app_iwdg` asleep, CFSR = 0): the network had stopped, not the CPU; the
trap's own halt then broke the Wi-Fi association. Since 00:48 the trap reads
`wdt_feeds` (0x200238c4) and DHCSR.S_LOCKUP through the AP every second without halting
and dumps only when feeding stops for > 9 s — the actual precondition of an IWDG reset.

### Side finding: no network while the upload is hashed

00:44:33–00:46:06: console and every request silent right after the last chunk; the
dump shows `web_v1_jobs` (prio 10) in `mbedtls_internal_sha256_process` and CHIP,
`network`, `coprocessor`, `sysupd_tick` ready but not running — `CONFIG_TIMESLICING=n`,
so the hashing job keeps every thread of priority 10 and lower off the CPU for the
whole verify (74 s over Ethernet earlier). `http_server_tid` waited in
`net_buf_alloc_fixed`, `esp_hosted_tx` in `esp_hosted_mcu_spi_transfer`; the Wi-Fi
association dropped and came back after the trap resumed the core.

### Side finding: Ethernet dies for good after RX pool exhaustion (W5500 driver)

During Wi-Fi uploads the console shows bursts of `net_pkt: Data buffer (N) allocation
failed`; `net mem` afterwards: RX DATA 160/160 max used. At 00:21:56 both the trap
and the host lost Ethernet (192.168.88.21) while Wi-Fi recovered:

- dump: `eth_w5500` and `rx_q[0]` blocked in `k_mem_slab_alloc` (pool empty), the
  current thread `web_v1_jobs` in `mbedtls_internal_sha256_process` (verify), IWDG fed;
- later, pools free again (80/80, 160/160), `eth_w5500` waits on `int_sem`, INT pin
  PA15 = 1 (inactive), Ethernet still dead, `net_if: iface 1 … send failure -5`.

`zephyr/drivers/ethernet/eth_w5500.c` `w5500_rx()`: when
`net_pkt_rx_alloc_with_buffer(…, K_MSEC(CONFIG_ETH_W5500_TIMEOUT))` fails it returns
without advancing `S0_RX_RD` or issuing `S0_CR_RECV`, after `w5500_check_for_ir()` has
already cleared `S0_IR.RECV`; the `-EAGAIN` branch of `w5500_thread_interrupt()` only
re-reads the PHY link. The frame stays in the W5500 buffer and nothing reads it again.
Not related to the watchdog (the kernel keeps running and feeding the IWDG). Not
changed: W5500 driver edits are the owner's call (all were reverted on 2026-09-15).
