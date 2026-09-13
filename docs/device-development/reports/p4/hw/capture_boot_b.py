"""Board B only: J-Link 000941000024, console /dev/cu.usbmodem5AE60208891.

Reset the board through J-Link and record its console for a while. Nothing is
sent to the board; each line is prefixed with host seconds since the reset, so
a fault can be placed on the timeline even when the board's own clock stops.

usage: capture_boot_b.py <out.log> <listen seconds>
"""
import os
import subprocess
import sys
import tempfile
import time

import serial

OUT, LISTEN = sys.argv[1], float(sys.argv[2])
UART = "/dev/cu.usbmodem5AE60208891"
JLINK = ["JLinkExe", "-USB", "000941000024", "-device", "STM32U585AI", "-if", "SWD",
         "-speed", "4000", "-autoconnect", "1", "-NoGui", "1", "-ExitOnError", "1"]

port = serial.Serial()
port.port, port.baudrate, port.timeout = UART, 115200, 0.1
port.dtr = port.rts = False
port.open()
port.reset_input_buffer()

with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as f:
    f.write("r\ng\nq\n")
    script = f.name
reset = subprocess.run(JLINK + ["-CommanderScript", script], capture_output=True, text=True)
os.unlink(script)
t0 = time.time()
if reset.returncode != 0:
    sys.exit("J-Link reset failed:\n" + reset.stdout[-2000:])

lines, partial = [], ""
while time.time() - t0 < LISTEN:
    chunk = port.read(4096)
    if not chunk:
        continue
    partial += chunk.decode("utf-8", "replace").replace("\r", "")
    *done, partial = partial.split("\n")
    stamp = time.time() - t0
    lines.extend(f"[+{stamp:7.2f}] {line}" for line in done)
port.close()
if partial:
    lines.append(f"[+{time.time() - t0:7.2f}] {partial}")

with open(OUT, "w") as f:
    f.write("\n".join(lines) + "\n")
print(f"{len(lines)} lines in {LISTEN:.0f} s -> {OUT}")
for line in lines:
    if any(k in line for k in ("FATAL", "FAULT", "Halting", "Faulting", "Current thread",
                               "BFAR", "Start main app", "web interface", "IPv4", "DHCP")):
        print(line[:200])
