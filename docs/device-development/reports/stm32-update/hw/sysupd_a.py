# SPDX-License-Identifier: Apache-2.0
"""Board A only (ST-Link 002F002B3233510739363634): step 0 of the STM32 update on the
production MCUboot, driven through the `sysupd` shell commands - no API.

usage:
  sysupd_a.py <workdir> flash <zephyr.signed.bin>     write slot 1 over ST-Link (external
                                                       loader), reset, record the boot
  sysupd_a.py <workdir> cmd "<shell command>" [secs]   send one command, record the answer
  sysupd_a.py <workdir> reset <seconds>                ST-Link hardware reset, record the boot
  sysupd_a.py <workdir> listen <seconds>               record the console only

Every run appends to <workdir>/console.log (raw) and <workdir>/runs.log (one line per
action with host time). Console lines carry host seconds since the start of the run.
"""

import hashlib
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../../../../tests/bench"))
import bench_console  # noqa: E402

CLI = ("/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/"
       "STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI")
SN = bench_console.ST_LINK_SERIAL
HERE = os.path.dirname(os.path.abspath(__file__))
STLDR = os.path.abspath(os.path.join(HERE, "../../../../../",
                                     "CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"))

WORK, CMD, ARGS = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(WORK, exist_ok=True)


def runs(text):
    with open(os.path.join(WORK, "runs.log"), "a") as f:
        f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {text}\n")
    print(text, flush=True)


def cli(*args, timeout=180):
    # mode=NORMAL: UR and HOTPLUG fail with "Unable to get core ID" on this board (P2).
    out = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", *args],
                         capture_output=True, text=True, timeout=timeout)
    tail = "\n".join(line for line in out.stdout.splitlines()[-6:] if line.strip())
    return out.returncode, tail


def dump(console, since, t0):
    for t, text in console.lines_since(since):
        print(f"{t - t0:8.2f} {text}")


console = bench_console.Console(os.path.join(WORK, "console.log"))
time.sleep(0.5)
t0 = time.time()
start = console.mark()

if CMD == "flash":
    image = ARGS[0]
    digest = hashlib.sha256(open(image, "rb").read()).hexdigest()
    runs(f"flash {image} sha256 {digest}")
    rc, tail = cli("-el", STLDR, "-w", image, "0x90000000", "-v", timeout=300)
    runs(f"write rc {rc}: {tail}")
    if rc == 0:
        rc, tail = cli("-hardRst")
        runs(f"reset rc {rc}")
        console.wait_for(r"Start main app", start, 60)
        time.sleep(25)
elif CMD == "reset":
    rc, tail = cli("-hardRst")
    runs(f"reset rc {rc}")
    time.sleep(float(ARGS[0]))
elif CMD == "cmd":
    wait = float(ARGS[1]) if len(ARGS) > 1 else 5.0
    runs(f"cmd {ARGS[0]!r}")
    # The console has dropped input bytes (board B); type slowly.
    for ch in ARGS[0]:
        console._ser and console._ser.write(ch.encode())
        time.sleep(0.02)
    console._ser and console._ser.write(b"\r\n")
    time.sleep(wait)
elif CMD == "listen":
    time.sleep(float(ARGS[0]))
else:
    sys.exit(f"unknown command {CMD}")

dump(console, start, t0)
console.close()
