"""Board B only: step 0 of the STM32 web update (reports/p6/stm32-ota-proposal.md) — does the
MCUboot on this board swap slot0 (OCTOSPI) with slot1_partition (SPI NOR) through the 64 KB
scratch_partition, revert an unconfirmed image, and resume a swap cut by a reset? No application
code: the bench MCUboot with hw/bench-mcuboot/mcuboot-cdc-recovery-direct.conf writes slot 2 and
marks it for test.

usage:
  ota_step0_b.py <workdir> mcuboot <zephyr.hex> <zephyr.bin>   J-Link: write and verify MCUboot
  ota_step0_b.py <workdir> list <name>                          reset, recovery `image list`
                                                                (MCUboot runs a pending swap or revert
                                                                BEFORE its recovery wait: recovery
                                                                answers only after it, ~37 s, and the
                                                                list shows the state after it; the
                                                                15 s search gives up first, but its
                                                                queued requests keep MCUboot in
                                                                recovery - found in step 0)
  ota_step0_b.py <workdir> upload2 <signed.bin>                 reset, upload to slot 2, `image test`
  ota_step0_b.py <workdir> boot <name> <seconds> [cut_after_s]  reset and record the boot; with
                                                                cut_after_s a J-Link reset that many
                                                                seconds after the first one

Console lines carry the host's seconds since the first reset of the run.
"""
import hashlib
import re
import subprocess
import sys
import threading
import time

import serial
import serial.tools.list_ports

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b  # noqa: E402

RECOVERY_SERIAL = "3543501200210047"
MCUMGR = "/Users/kiro/go/bin/mcumgr"
MTU = 4096

WORK, CMD, ARGS = sys.argv[1], sys.argv[2], sys.argv[3:]


class Capture:
    def __init__(self):
        self.lines = []
        self.buf = b""
        self.t0 = None
        self.stop = threading.Event()
        self.lock = threading.Lock()
        threading.Thread(target=self.run, daemon=True).start()
        time.sleep(0.3)

    def run(self):
        board_b.refuse(board_b.CONSOLE)
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = board_b.CONSOLE, 115200, 0.05
        s.dtr = s.rts = False
        s.open()
        while not self.stop.is_set():
            d = s.read(4096)
            if not d:
                continue
            now = time.monotonic()
            with self.lock:
                self.buf += d
                while b"\n" in self.buf:
                    line, self.buf = self.buf.split(b"\n", 1)
                    rel = now - self.t0 if self.t0 else 0.0
                    self.lines.append(f"{rel:8.3f} {line.decode('utf-8', 'replace').rstrip()}")
        s.close()

    def mark(self):
        with self.lock:
            self.t0 = time.monotonic()

    def note(self, text):
        rel = time.monotonic() - self.t0 if self.t0 else 0.0
        with self.lock:
            self.lines.append(f"{rel:8.3f} ## {text}")
        print(f"{rel:8.3f} ## {text}", flush=True)

    def save(self, name):
        self.stop.set()
        time.sleep(0.2)
        with self.lock:
            text = "\n".join(self.lines) + "\n"
        open(f"{WORK}/{name}.console.log", "w").write(text)
        return text


def mcumgr(port, *a, t=2, r=1):
    return subprocess.run([MCUMGR, "--conntype", "serial", "--connstring",
                           f"dev={port},baud=115200,mtu={MTU}", "-t", str(t), "-r", str(r), *a],
                          capture_output=True, text=True)


def reset(cap):
    r = board_b.jlink("r\ng\n")
    if r.returncode != 0:
        sys.exit("J-Link reset failed:\n" + r.stdout[-1500:])


def find_recovery(cap, limit=15):
    t = time.monotonic()
    while time.monotonic() - t < limit:
        for p in serial.tools.list_ports.comports():
            if p.serial_number == RECOVERY_SERIAL:
                board_b.refuse(p.device)
                c = mcumgr(p.device, "image", "list")
                if c.returncode == 0 and "Images:" in c.stdout:
                    cap.note(f"recovery on {p.device}")
                    return p.device, c.stdout
        time.sleep(0.1)
    return None, None


if CMD == "mcuboot":
    hexf, binf = ARGS
    r = board_b.jlink(f"loadfile {hexf}\nverifybin {binf} 0x08000000\nr\ng\n")
    open(f"{WORK}/mcuboot-jlink.log", "w").write(r.stdout)
    ok = "Verify successful." in r.stdout
    print(f"J-Link rc={r.returncode} verify={'ok' if ok else 'FAILED'}")
    sys.exit(0 if ok and r.returncode == 0 else 1)

cap = Capture()
cap.mark()
reset(cap)

if CMD == "list":
    port, out = find_recovery(cap)
    cap.note("image list:\n" + (out or "recovery did not answer"))
    cap.save(ARGS[0])
    sys.exit(0 if port else 1)

if CMD == "upload2":
    img = ARGS[0]
    sha = hashlib.sha256(open(img, "rb").read()).hexdigest()
    port, out = find_recovery(cap)
    if not port:
        cap.save("upload2")
        sys.exit("recovery did not answer")
    cap.note("before:\n" + out)
    t = time.monotonic()
    up = mcumgr(port, "image", "upload", "-n", "2", img, t=10, r=3)
    cap.note(f"upload -n 2 {img} sha256 {sha}: rc={up.returncode} in {time.monotonic() - t:.0f} s "
             f"{up.stdout[-300:]} {up.stderr[-300:]}")
    out = mcumgr(port, "image", "list", t=5).stdout
    cap.note("after upload:\n" + out)
    # The slot 1 entry's hash, as MCUboot reports it.
    m = re.search(r"slot=1.*?hash:\s*([0-9a-f]{64})", out, re.S)
    if not m:
        cap.save("upload2")
        sys.exit("no slot=1 image in the list")
    # MCUboot validates the hash of every slot before it sets the state (bs_set): with
    # t=5 the first run timed out.
    tst = mcumgr(port, "image", "test", m.group(1), t=30)
    cap.note(f"image test {m.group(1)}: rc={tst.returncode}\n{tst.stdout}{tst.stderr}")
    cap.save("upload2")
    sys.exit(tst.returncode)

if CMD == "boot":
    name, seconds = ARGS[0], float(ARGS[1])
    cut = float(ARGS[2]) if len(ARGS) > 2 else None
    cap.note(f"reset, recording {seconds:.0f} s" + (f", cut at {cut:.1f} s" if cut else ""))
    if cut:
        time.sleep(cut)
        cap.note("J-Link reset (cut)")
        reset(cap)
        time.sleep(max(0.0, seconds - cut))
    else:
        time.sleep(seconds)
    text = cap.save(name)
    print(text[-6000:])
    sys.exit(0)

sys.exit(f"unknown command {CMD}")
