"""Console capture of a bench board while the owner acts in Apple Home (no flash, no wipe, no commands).

usage: console_capture.py <tag> [minutes]
env:   APPLE_CONSOLE_SERIAL — USB serial of the console (board B: 5AE6020889; default board A's ST-LINK)

2026-09-15, owner: "why does it take so long between tapping the relay in the Home app and the relay switching".
Every console line goes with host time into reports/p3/apple-home/<YYYYmmdd-HHMM>-<tag>/session.log until <minutes>
pass or a file named STOP appears in that directory; the raw stream is kept as console.log.gz.
"""
import gzip, os, shutil, sys, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[4]
sys.path.insert(0, str(REPO / "tests/bench"))
from bench_console import Console  # noqa: E402

tag = sys.argv[1]
minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 60
serial = os.environ.get("APPLE_CONSOLE_SERIAL")
OUT = HERE / f"{time.strftime('%Y%m%d-%H%M')}-{tag}"
OUT.mkdir(parents=True, exist_ok=True)
console = Console(str(OUT / "console.raw"), **({"usb_serial": serial} if serial else {}))
log = open(OUT / "session.log", "w", buffering=1)


def stamp(t):
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t % 1 * 1000):03d}"


log.write(f"{stamp(time.time())}  ### capture start, console {serial or 'ST-LINK'}\n")
print(f"CAPTURING {OUT}", flush=True)
seen = console.mark()
deadline = time.time() + minutes * 60
while time.time() < deadline and not (OUT / "STOP").exists():
    time.sleep(0.2)
    lines = console.lines_since(seen)
    seen += len(lines)
    for t, text in lines:
        log.write(f"{stamp(t)}  {text}\n")
log.write(f"{stamp(time.time())}  ### capture end\n")
print("capture end", flush=True)
console.close()
log.close()
raw = OUT / "console.raw"
with open(raw, "rb") as src, gzip.open(OUT / "console.log.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)
raw.unlink()
