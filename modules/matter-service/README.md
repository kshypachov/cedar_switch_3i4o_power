# matter-service

What the web interface may know about the Matter stack, and ask of it: the
stack's state, the commissioning window, the onboarding codes, and the fabric
table.

Contract: "Matter" in
[`api-contract.md`](../../docs/device-development/api-contract.md) (with the
decisions P3 recorded there), the `MatterStatus`, `CommissioningWindow`,
`OnboardingCodes`, `Fabric` and `Capabilities` schemas in
[`openapi.json`](../../docs/device-development/openapi.json), and sections 3
and 6 of the development plan. The header carries the design; this file covers
how it fits the system and what was measured.

## Shape

Three layers, so that the part with decisions in it runs in the sim tier:

| Layer | Where | Language | Tested |
|---|---|---|---|
| Core: snapshot, request slots, window ownership, limits | `lib/matter_service.c` | C, no Matter types | `tests/matter_service` (sim) |
| Adapter: the platform on the SDK, the AppDelegate, the fabric-table delegate | `src/matter/matter_service_chip.cpp` | C++ | on the board |
| HTTP binding: six operations, jobs, error codes | `src/web/api/v1/matter.c` | C | `tests/web_api` (`test_matter_*`, sim) |

`src/matter/matter_init.cpp` reports the stack starting and started (or the
error it failed with), installs the AppDelegate before `Server::Init()`, and
the fabric delegate after it. `main.c` installs the platform before the network
comes up, because the first IPv6 address starts the stack.

## Threads

- **Readers never wait for Matter.** Every GET answers from a snapshot under a
  mutex held for the copy. The HTTP server's thread is cooperative (P2), and
  the Matter thread spends seconds in some SDK calls on this board (below): a
  handler that waited would stop every client and the shell with it.
- **Changes are requests.** Open and close return at once; the work runs on
  the Matter thread through `ScheduleWork`, and a callback on that thread
  finishes the job. Accepted requests take one of
  `CONFIG_MATTER_SERVICE_REQUEST_SLOTS` slots until they run.
- **The snapshot follows events.** Stack started, window opened or closed,
  fabric committed, updated or removed. The adapter re-reads the window from a
  work item of its own rather than inside the SDK's callback: the SDK tells the
  AppDelegate the window closed before it marks it closed (see the adapter's
  header comment), and a read inside the callback kept a finished window open
  on the page.

## Decisions worth knowing before changing it

- **Who opened the window is remembered here.** The SDK reports basic or
  enhanced only for a window a controller opened through the Administrator
  Commissioning cluster; one the device opened reads as "not opened via the
  cluster". So `source` is `controller` when the SDK names an opener, `web`
  for the window this service opened, `local` for anything else (the debug
  shell's `matter commissioning open`).
- **Remaining time only for our own window.** The SDK does not expose the
  timeout a controller or the shell asked for, so those windows report 0 and
  the page shows no timer rather than an invented one.
- **Codes for any basic window; never for an enhanced one.** A basic window
  uses the device's own passcode, whoever opened it. An enhanced window
  carries a controller's verifier; the factory codes would be wrong.
- **A second open is refused, including an accepted open that has not run.**
  An open that finds a window opened elsewhere by the time it runs fails its
  job with `invalid_state` instead of replacing it.
- **The published window range** is the contract's 180-900 s narrowed by the
  SDK's own limits; on the pinned SDK they are equal (3 min; 15 min without
  extended advertising).
- **The fabric id** is 16 hex digits of the SHA-256 of the fabric's root public
  key, a colon, and the fabric id: a fabric index is reused after removal and a
  fabric id is picked by the controller.
- **Nothing here logs a code.** The debug shell does, on purpose, and stays
  (owner's decision, plan section 13).

## Kconfig

| Option | Default | Why |
|---|---|---|
| `CONFIG_MATTER_SERVICE_MAX_FABRICS` | 5 | the SDK's table on Zephyr (`CHIP_CONFIG_MAX_FABRICS`); the adapter refuses to build if the snapshot is smaller |
| `CONFIG_MATTER_SERVICE_REQUEST_SLOTS` | 4 | bounds a burst of accepted open/close requests; a full table is 429 |

The firmware also needs `CONFIG_CHIP_ETHERNET=y` - without it the SDK builds no
DNS-SD at all - and `CONFIG_NET_IF_MCAST_IPV6_ADDR_COUNT=14` (see `prj.conf`).

## Measured on the board (P3)

`docs/device-development/reports/p3/` has the logs.

- Before P3 every boot logged `DNS-SD advertising not available`: the build had
  `chip_mdns = "none"` because neither `CONFIG_CHIP_ETHERNET` nor
  `CONFIG_CHIP_WIFI` was set. With Ethernet the SDK's minimal mDNS advertises
  `_matterc._udp` for an open window and `_matter._tcp` per fabric; the host
  sees both.
- chip-tool commissioned the board twice, as two commissioners with different
  roots: 65 s and 76 s, the second through a window opened with the API. Both
  fabrics are listed with distinct ids; the first survived a reset.
- Reads of every Matter resource take about 70 ms, also while a window is
  being opened.
- Opening a window held the Matter thread for 11-18 s ("Long dispatch time"
  in the SDK's log) while the request itself was answered in about 95 ms. The
  adapter's timing put it on generating the onboarding codes: 5-6 s per call,
  and the first versions of the service generated them on every window
  re-read. Why that call is slow on this board is not established.
- That cost broke commissioning once the window was re-read after the SDK's
  callbacks: a re-read during PASE blocked the Matter thread for 5 s and
  chip-tool gave up on `SendNOC` (fail-safe then removed the half-added
  fabric). The codes are now generated once, when the stack starts, and kept.
- The Matter thread and web-auth's derivation thread both ran at preemptive
  priority 14 with time slicing off; the firmware now runs the derivation at 13
  (`prj.conf`). A sign-in still sometimes exceeded the browser's 10 s while
  Matter was busy with storage, so this is not the cause; it is open.
- The stack started 2.4 s after boot once the 5 s sleep left the net_mgmt
  callback. Its initialisation took 20 s with no fabric and 30 s with one.

## Testing

- `tests/matter_service` (sim, 29 cases): lifecycle, accepted-but-not-run
  opens, limits (narrower and wider SDK), refusals from the stack, windows
  opened by controllers and the shell, codes and their failure, fabric order,
  bounds and termination, and that no read touches the platform.
- `tests/web_api` (`test_matter_*`, sim, 10 cases): the six operations through
  the route table with a fake platform - exact bodies, 409/422/503 before the
  job, key reuse, job success and failure, escaping and hex.
- `src/web/frontend`: component tests of the Matter screen and an end-to-end
  run against the mock.

```sh
tests/ci/run-sim-tests.sh -s cedar.matter_service -s cedar.web_api
```
