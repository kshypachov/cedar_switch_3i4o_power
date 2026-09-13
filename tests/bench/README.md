# Bench scripts

Host-side measurement scripts for the board on the bench. They are not Twister
suites and have no `testcase.yaml`, so neither Twister nor CI picks them up;
they need the board, the ST-LINK and the wired link to it.

| Script | What it does | Changes board state |
|---|---|---|
| `boot_cycle.py` | A series of warm (debugger NRST) or cold (hands on the supply) starts, each measured | resets the MCU; reads only |
| `smoke.py` | Unattended soak: shell latency, ping and HTTP for a fixed time | nothing |
| `bench_console.py` | Shared console: finds the port by ST-LINK serial, survives VCP drops, timestamps every line | — |

Run them with the workspace venv (it has `pyserial`):

```sh
cd tests/bench
/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python -u boot_cycle.py OUT --warm 10
/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python -u boot_cycle.py OUT --cold 10 --first-index 11 --cold-wait 3600
/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python -u smoke.py OUT --duration 7200
```

HTTP readiness is probed by waiting for ping and then sending one request at a
time with a long timeout. Since P2 the request is `GET /api/v1/auth/state`: the legacy REST routes
it used to poll were removed. Polling with short timeouts keeps this server busy:
it has four client slots and a 10 s inactivity timeout, and on 2026-09-12 a
request every ~2 s with a 2 s timeout got no answer for 180 s.

Only one script can hold the console at a time. CLion's serial monitor counts:
if the port is busy, `lsof /dev/cu.usbmodem*` shows who has it.

## Bench facts the scripts rely on

- ST-LINK V3SET serial `002F002B3233510739363634`. The `/dev/cu.usbmodem*`
  number changes between sessions; the second ST-LINK on the bench belongs to
  another board. The board's own USB CDC (VID `0x2fe3`) is the C6 UART bridge.
- The board is `192.168.88.14` and the host reaches it on `en7`. `smoke.py`
  records the route interface; a run over Wi-Fi is not comparable.
- Warm reset is `STM32_Programmer_CLI -c port=swd sn=… mode=NORMAL -hardRst`.
  `mode=UR` and `HOTPLUG` fail with "Unable to get core ID".

## The latency baseline and how it was measured

The 2026-09-12 numbers in `docs/upstream/w5500-filtering-research.md` and in
section 9 of the plan (HTTP p50 53 ms / p95 56 ms, ping 8.5–11.6 ms, no loss)
were taken with these commands, recovered from the session that took them;
the research note gives the numbers but not the method:

```sh
ping -c 20 -i 0.3 -W 2000 192.168.88.14
for i in $(seq 15); do curl -s -m 8 -o /dev/null -w "%{time_total}\n" http://192.168.88.14/ ; done \
  | sort -n | awk '{a[NR]=$1} END{printf "min=%.3f p50=%.3f p95=%.3f max=%.3f\n", a[1], a[int(NR*0.5)+1], a[int(NR*0.95)], a[NR]}'
```

`smoke.py` repeats exactly that: the same ping options, fifteen sequential
requests of `/` per round, and the same index arithmetic for p50 and p95
(the 8th and 14th of 15 sorted samples). Change any of it and the comparison
with the baseline stops meaning anything.

## What cannot be measured from here

- **Boot reason.** Matter's `DiagnosticDataProvider` reads and clears the RCC
  reset flags during init, and counts a pin reset as a power-on reboot; by the
  time the shell answers, `RCC_CSR` (`0x46020CF4`) is clear. The mode of each
  start is whatever the script did.
- **Heap and network counters.** The build has
  `CONFIG_SYS_HEAP_RUNTIME_STATS=n` and `CONFIG_NET_STATISTICS=n`; the snapshots
  try `kernel heap` and `net stats` anyway and keep whatever comes back.
- **What a relay physically does during reset.** The scripts read the output
  register bits of PA5, PC4, PC5 and PB2 after boot. The coils are driven
  through a ULN2003, so a bit at 1 means an energised coil; what happens between
  reset and GPIO init needs eyes on the relay LEDs or a logic analyser.

## What changed under the baseline in P2

`smoke.py` still measures `GET /` the P0 way. Since P2 that URL is the browser
application served by web-assets through a dynamic resource (ETag,
Cache-Control, a Content-Security-Policy and five more headers), not the legacy
static resource the 2026-09-12 baseline measured. On the same board the P0
method gave p50 125 / p95 137 ms for `/` against the baseline's 53 / 56, and
p50 89 / p95 97 ms for `/api/v1/auth/state`. A smoke run after P2 is comparable
with other runs after P2, not with the baseline; details are in
`docs/device-development/reports/p2`.
