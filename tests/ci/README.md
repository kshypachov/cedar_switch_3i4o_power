# Simulated test tier

Zephyr's POSIX architecture (`native_sim`) does not build on macOS — the
architecture's own CMake refuses with "The POSIX architecture only works on
Linux". Since the development hosts here are macOS, the sim tier runs in a
small Linux container instead of being downgraded to whatever happens to run
natively.

## Running

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

## Scope

This tier covers logic with no hardware dependency. Anything touching SPI,
UART, the W5500, the ESP32-C6, MCUboot timings or power loss belongs to the
hardware tier and lives in the other `tests/` directories, which require the
board. Section 12 of the development plan defines the split.
