# Test runners

Three runners live here: `run-sim-tests.sh` for the simulated tier, which needs
a Linux container; `run-contract-tests.sh` for the API contract and the
web-assets generator; and `run-frontend-tests.sh` for the browser application.
The last two need no container.

## Why the sim tier needs a container

Zephyr's POSIX architecture (`native_sim`) does not build on macOS — the
architecture's own CMake refuses with "The POSIX architecture only works on
Linux". Since the development hosts here are macOS, the sim tier runs in a
small Linux container instead of being downgraded to whatever happens to run
natively.

## Running the sim tier

```sh
colima start --cpu 4 --memory 8 --disk 60 --mount /Volumes/Programming:w
tests/ci/run-sim-tests.sh
```

The first run builds the image (a few minutes); later runs reuse it. Extra
arguments are passed straight to twister, so a single suite is:

```sh
tests/ci/run-sim-tests.sh -s cedar.job_manager
```

## What is in the image

Debian trixie plus a build toolchain and twister's Python dependencies. Two
version floors forced trixie over bookworm: Zephyr needs CMake ≥ 3.28
(bookworm has 3.25) and Python ≥ 3.12 (bookworm has 3.11).

No Zephyr SDK. `native_sim` compiles with the container's own gcc, which is
why `ZEPHYR_TOOLCHAIN_VARIANT=host` is set in the image — without it Zephyr
hunts for an SDK that is deliberately absent. The result is roughly 1 GB
rather than the ~10 GB of `zephyrprojectrtos/ci`.

The platform is `native_sim/native/64`, not plain `native_sim`: on Apple
silicon the container is arm64 with a 64-bit-only userspace, where the 32-bit
default cannot link.

## The contract runner

```sh
tests/ci/run-contract-tests.sh            # both halves, one command
tests/ci/run-contract-tests.sh -k network # extra arguments go to pytest
```

This one needs no container: it is plain Python, and it runs the checks on
`openapi.json` and the mock server's suite, which is also the contract test.
The tooling and the decisions behind it are in
[`tools/api-contract/README.md`](../../tools/api-contract/README.md). It prefers
the workspace venv and falls back to `python3`, so it also works on a CI runner
with no west workspace.

## The frontend runner

```sh
tests/ci/run-frontend-tests.sh             # npm ci, vitest, build, e2e
SKIP_E2E=1 tests/ci/run-frontend-tests.sh  # stop after the build
```

It installs exactly the lockfile, generates the API types from `openapi.json`,
runs the unit and component tests, builds the application (which checks the
512 KiB gzip budget), and runs the Playwright suite against the mock server with
`--ui` serving the build. The e2e step uses the installed Chrome by default; CI
sets `PW_CHANNEL=` and installs Playwright's Chromium. Details in
[`src/web/frontend/README.md`](../../src/web/frontend/README.md).

## Suites added in P2

| Suite | What it drives |
|---|---|
| `cedar.web_auth` | web-auth's state machine with every platform function faked |
| `cedar.web_api` | the strict JSON reader and writer, the middleware on a test router, the real v1 bindings over web-auth |
| `cedar.web_api_http` | web-api's adapter through Zephyr's own HTTP server over loopback, with the firmware's resource list and a generated asset table |
| `cedar.web_assets` | the static response policy on a hand-built table |

`cedar.web_api_http` runs a real server inside native_sim and talks to it over
the loopback interface, so it needs nothing from the host network; it is also
the slowest of the set (about 20 s of tests).

## GitHub Actions

`.github/workflows/checks.yml` runs all three on every push and pull
request — the owner's decision of 2026-09-12, recorded in section 12 of the
development plan.

The sim job there does not use this container. It clones Zephyr shallow at a
pinned revision, runs `west init -l` **without** `west update`, and calls twister
directly. Two things about that were measured rather than assumed, and both are
worth knowing before editing the workflow:

- A west *workspace* is required even though no manifest project is. Outside one,
  Zephyr's `zephyr_module.cmake` takes its `else()` branch and ignores
  `ZEPHYR_EXTRA_MODULES` entirely; the build then fails with "undefined symbol
  JOB_MANAGER" and nothing naming the real cause. `west init -l` creates the
  workspace without fetching anything, and all 124 cases pass that way on a clean
  upstream tree.
- Twister cannot be pointed at `tests/` as a whole on a clean Zephyr. The
  hardware suites name `cedar_switch_3in4out_power_rev3/...` in `platform_allow`,
  and that board lives untracked in the shared Zephyr tree rather than in this
  repository. `platform_allow` is validated while testsuites are still being
  discovered, so the whole run aborts before any filtering — `--tag sim` and
  `--exclude-tag hw` were both tried and neither helps. The workflow therefore
  derives its roots from the `sim` tag.

## Scope

This tier covers logic with no hardware dependency. Anything touching SPI,
UART, the W5500, the ESP32-C6, MCUboot timings or power loss belongs to the
hardware tier and lives in the other `tests/` directories, which require the
board. Section 12 of the development plan defines the split.
