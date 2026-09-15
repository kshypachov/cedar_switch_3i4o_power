"""Run `matter crypto_bench` on one image and record the timings.

usage: crypto_bench_run.py <name> <zephyr.signed.bin> [iterations]

Flashes the image (app only), waits for Matter, sends `matter crypto_bench N` with the
echo checked (the console drops input bytes now and then), collects the result lines
("<op> total=... us avg=... us iter=N") up to "P-256 ECDSA verify".

Results in reports/crypto/bench/<name>/: console.log.gz, bench.json.
"""
import gzip, json, re, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[4]
sys.path.insert(0, str(REPO / "tests/bench"))
from bench_console import Console  # noqa: E402

CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
EL = REPO / "CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"
SN = "002F002B3233510739363634"
ROW = re.compile(r"(SHA-256 512B|HMAC-SHA256 512B|HKDF-SHA256 32B|AES-CCM enc 128B|AES-CCM dec 128B|P-256 keygen|P-256 ECDH|P-256 ECDSA sign|P-256 ECDSA verify)\s+total=(\d+) us avg=(\d+) us iter=(\d+)")

name, image = sys.argv[1], Path(sys.argv[2])
iterations = int(sys.argv[3]) if len(sys.argv) > 3 else 10
OUT = HERE / name
OUT.mkdir(parents=True, exist_ok=True)
console = Console(str(OUT / "console.raw"))
time.sleep(1.0)
result = dict(name=name, image=str(image), iterations=iterations, started=time.strftime("%Y-%m-%d %H:%M:%S"))

w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", str(EL), "-w", str(image), "0x90000000", "-v"],
                   capture_output=True, text=True)
result["flash_ok"] = w.returncode == 0
if w.returncode == 0:
    mark = console.mark()
    subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
    result["matter_ready"] = console.wait_for(r"Matter stack initialized|Matter Server Init failed|Matter init failed", mark, 600) is not None
    time.sleep(20)
    cmd = f"matter crypto_bench {iterations}"
    sent = None
    for _ in range(4):
        mark = console.mark()
        console.send(cmd)
        if console.wait_for(re.escape(cmd) + r"\s*$", mark, 4) is not None:
            sent = mark
            break
        time.sleep(1.0)
    result["sent"] = sent is not None
    if sent is not None:
        end = console.wait_for(r"P-256 ECDSA verify\s+total=", sent, 1800)
        result["finished"] = end is not None
        time.sleep(1)
        rows = {}
        for _, text in console.lines_since(sent):
            m = ROW.search(text)
            if m:
                rows[m[1]] = dict(total_us=int(m[2]), avg_us=int(m[3]), iterations=int(m[4]))
            elif re.search(r"<err>|error|failed", text, re.I) and "crypto" in text.lower():
                result.setdefault("errors", []).append(text.strip()[-160:])
        result["rows"] = rows
console.close()
raw = OUT / "console.raw"
with open(raw, "rb") as src, gzip.open(OUT / "console.log.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)
raw.unlink()
(OUT / "bench.json").write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
print(json.dumps({k: result.get(k) for k in ("name", "matter_ready", "finished", "rows")}, ensure_ascii=False))
