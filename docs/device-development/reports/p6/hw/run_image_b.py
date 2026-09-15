"""Board B only: put an image into slot0 through the bench MCUboot's CDC recovery and
record the console of the boot that follows.

Copy of reports/p5/hw/run_image_b.py (itself from reports/p4/hw/run_test_image_b.py): the recovery port is found by
the exact USB serial of the bench MCUboot (3543501200210047). A substring search would
also match the application's CDC, whose serial ends with the same digits.

usage: run_image_b.py <workdir> <zephyr.signed.bin> <listen seconds>
"""
import hashlib
import subprocess
import sys
import threading
import time

import serial
import serial.tools.list_ports

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b  # noqa: E402

WORK, IMG, LISTEN = sys.argv[1], sys.argv[2], float(sys.argv[3])
RECOVERY_SERIAL = "3543501200210047"
MCUMGR = "/Users/kiro/go/bin/mcumgr"
# 512 for the bench MCUboot of 2026-09-13; 4096 once hw/bench-mcuboot/mcuboot-cdc-recovery-fast.conf
# is on the board (BOOT_SERIAL_MAX_RECEIVE_SIZE 8192).
MTU = int(__import__("os").environ.get("P6_MCUMGR_MTU", "512"))

sha = hashlib.sha256(open(IMG, "rb").read()).hexdigest()
print(f"image {IMG} sha256 {sha}", flush=True)

log = bytearray()
lock = threading.Lock()
stop = threading.Event()


def reader():
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = board_b.CONSOLE, 115200, 0.1
    s.dtr = s.rts = False
    s.open()
    while not stop.is_set():
        d = s.read(4096)
        if d:
            with lock:
                log.extend(d)
    s.close()


def recovery_ports():
    """Every port with the bench MCUboot's serial. macOS can list two for it (seen after
    the CDC had been opened and closed repeatedly): only one answers mcumgr."""
    found = []
    for p in serial.tools.list_ports.comports():
        if p.serial_number == RECOVERY_SERIAL:
            board_b.refuse(p.device)
            found.append(p.device)
    return found


def mcumgr(port, *a, t=1, r=1):
    return subprocess.run([MCUMGR, "--conntype", "serial", "--connstring",
                           f"dev={port},baud=115200,mtu={MTU}", "-t", str(t), "-r", str(r), *a],
                          capture_output=True, text=True)


threading.Thread(target=reader, daemon=True).start()
time.sleep(0.3)
reset = board_b.jlink("r\ng\n")
print(f"J-Link reset rc={reset.returncode}", flush=True)
if reset.returncode != 0:
    stop.set()
    sys.exit("J-Link reset failed:\n" + reset.stdout[-1500:])
t0 = time.time()
port = None
seen = set()
# With a valid image in slot 2 (STM32 update step 0, hw row 23) MCUboot hashes both slots for every
# `image list`, ~4 s: a 1 s timeout never saw the answer (2026-09-14 ~19:30, nothing written). A
# pending swap also runs before recovery answers (~37 s).
while time.time() - t0 < 60:
    for p in serial.tools.list_ports.comports():
        if p.serial_number:
            seen.add((p.device, p.serial_number))
    port = None
    # Two passes: the recovery wait is 5 s and macOS may list a dead second port first. A short
    # request on every port keeps MCUboot in recovery once one reaches it; the long pass then waits
    # for the answer (an 8 s first try on the dead port missed the window, ~19:40).
    candidates = recovery_ports()
    for timeout in (1, 8):
        for candidate in candidates:
            c = mcumgr(candidate, "image", "list", t=timeout)
            if c.returncode == 0 and "Images:" in c.stdout:
                port = candidate
                break
        if port:
            break
    if port:
        break
    time.sleep(0.1)
if not port:
    stop.set()
    time.sleep(0.3)
    with lock:
        console = bytes(log).decode("utf-8", "replace").replace("\r", "")
    open(f"{WORK}/image_console.log", "w").write(f"# recovery did not answer; image {sha} NOT written\n" + console)
    sys.exit(f"recovery did not answer; USB serials seen: {sorted(seen)}\nconsole:\n{console[-1500:]}")
print(f"recovery on {port} after {time.time() - t0:.1f} s", flush=True)
t1 = time.time()
up = mcumgr(port, "image", "upload", IMG, t=10, r=3)
print(f"upload rc={up.returncode} in {time.time() - t1:.0f} s", flush=True)
print(mcumgr(port, "image", "list", t=5).stdout, flush=True)
with lock:
    log.clear()
board_b.jlink("r\ng\n")
time.sleep(LISTEN)
stop.set()
time.sleep(0.3)
text = bytes(log).decode("utf-8", "replace").replace("\r", "")
open(f"{WORK}/image_console.log", "w").write(f"# image sha256 {sha}\n" + text)
print(text[-5000:])
