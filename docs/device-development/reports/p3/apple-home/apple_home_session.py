"""Apple Home commissioning session on the bench board, with the console logged.

usage: apple_home_session.py <zephyr.signed.bin> [minutes]

Owner's report (2026-09-14): commissioning from Apple Home takes much longer than
with chip-tool. The owner pairs by hand; this script prepares the board and records
everything the device prints meanwhile:
 1. flash the image as it ships, wait for Matter
 2. storage_wipe yes, reboot: pairing starts from an empty settings store
 3. matter commissioning open; the QR payload and manual code from the log go to
    qr.png / codes.txt (the owner scans qr.png)
 4. every console line with host time into session.log until <minutes> pass or a
    file named STOP appears in the session directory

Results in reports/p3/apple-home/<YYYYmmdd-HHMM>/: session.log, console.log.gz,
qr.png, codes.txt.
"""
import gzip, re, shutil, subprocess, sys, threading, time
from pathlib import Path

import qrcode

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[4]
sys.path.insert(0, str(REPO / "tests/bench"))
from bench_console import Console  # noqa: E402

CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
EL = REPO / "CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"
SN = "002F002B3233510739363634"

# Board B (J-Link, 2026-09-15): `apple_home_session.py --no-flash [minutes]` with APPLE_CONSOLE_SERIAL=5AE6020889 —
# the image is already on the board (bench MCUboot upload), step 1 becomes a shell reboot.
NO_FLASH = sys.argv[1] == "--no-flash"
image = None if NO_FLASH else Path(sys.argv[1])
minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 30
CONSOLE_SERIAL = __import__("os").environ.get("APPLE_CONSOLE_SERIAL")
OUT = HERE / time.strftime("%Y%m%d-%H%M")
OUT.mkdir(parents=True, exist_ok=True)
console = Console(str(OUT / "console.raw"), **({"usb_serial": CONSOLE_SERIAL} if CONSOLE_SERIAL else {}))
log = open(OUT / "session.log", "w", buffering=1)
done = threading.Event()


def stamp(t):
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t % 1 * 1000):03d}"


def note(text):
    log.write(f"{stamp(time.time())}  ### {text}\n")
    print(text, flush=True)


def writer():
    seen = console.mark()
    while not done.is_set():
        time.sleep(0.2)
        lines = console.lines_since(seen)
        seen += len(lines)
        for t, text in lines:
            log.write(f"{stamp(t)}  {text}\n")


def send_echoed(cmd, attempts=4):
    for _ in range(attempts):
        mark = console.mark()
        console.send(cmd)
        if console.wait_for(re.escape(cmd) + r"\s*$", mark, 4) is not None:
            return mark
        time.sleep(1.0)
    return None


threading.Thread(target=writer, daemon=True).start()
time.sleep(1.0)
if NO_FLASH:
    note(f"no flash: image already on the board, console {CONSOLE_SERIAL or 'ST-LINK'}")
else:
    note(f"flashing {image}")
    w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", str(EL), "-w", str(image), "0x90000000", "-v"],
                       capture_output=True, text=True)
    if w.returncode != 0:
        (OUT / "flash.log").write_text(w.stdout + w.stderr)
        note("flash failed")
        sys.exit(1)
    mark = console.mark()
    subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
    console.wait_for(r"Matter stack initialized|Matter Server Init failed|Matter init failed", mark, 600)
    time.sleep(3)

note("storage_wipe yes")
mark = send_echoed("storage_wipe yes")
if mark is None or console.wait_for(r"storage_wipe: offset .* erased rc=0", mark, 600) is None:
    note("wipe failed")
    sys.exit(1)
time.sleep(1)
for _ in range(4):
    mark = console.mark()
    console.send("kernel reboot cold")
    if console.wait_for(r"Starting bootloader", mark, 15) is not None:
        break
ready = console.wait_for(r"Matter stack initialized|Matter Server Init failed|Matter init failed", mark, 600)
note(f"boot on the empty store: {ready[1].string.strip() if ready else 'no Matter ready line'}")
time.sleep(5)

qr = code = None
for _ in range(3):
    mark = send_echoed("matter commissioning open")
    hit = console.wait_for(r"Matter QR code: (MT:\S+)", mark, 60) if mark is not None else None
    if hit:
        qr = hit[1].group(1)
        m = console.wait_for(r"Matter manual pairing code: (\d+)", mark, 10)
        code = m[1].group(1) if m else None
        break
if qr is None:
    note("no QR code in the log")
    sys.exit(1)
img = qrcode.make(qr, box_size=14, border=4)
img.save(OUT / "qr.png")
(OUT / "codes.txt").write_text(f"QR: {qr}\nmanual code: {code}\nwindow opened: {time.strftime('%H:%M:%S')}\n")
note(f"QR_READY {OUT / 'qr.png'} {qr} manual {code}")

deadline = time.time() + minutes * 60
while time.time() < deadline and not (OUT / "STOP").exists():
    time.sleep(1)
note("session end")
time.sleep(1)
done.set()
time.sleep(0.5)
console.close()
log.close()
raw = OUT / "console.raw"
with open(raw, "rb") as src, gzip.open(OUT / "console.log.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)
raw.unlink()
