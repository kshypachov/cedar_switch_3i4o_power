"""Idle thread load and storage read time of one image, without commissioning.

usage: threads_idle.py <name> <zephyr.signed.bin>

Question (2026-09-14, file/039): the same SPI_STM32_INTERRUPT=y config is 43.5 s on
the image built 09-13 19:12 and 59.6 s on the image built on today's sources
(eth_w5500.c changed); the polled base is unchanged. Crypto phases are slower too,
so something takes CPU time. Flash the image, start without touching the storage,
let the network settle, and take `kernel thread list` twice 60 s apart (per-thread
execution time deltas), then storage_bench 3 mt/cfg/unique-id.

Results in tuning/diag/<name>/: console.log.gz, threads.json.
"""
import gzip, json, re, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[5]
sys.path.insert(0, str(REPO / "tests/bench"))
from bench_console import Console  # noqa: E402

CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
EL = REPO / "CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"
SN = "002F002B3233510739363634"

name, image = sys.argv[1], Path(sys.argv[2])
OUT = HERE / name
OUT.mkdir(parents=True, exist_ok=True)
console = Console(str(OUT / "console.raw"))
time.sleep(1.0)
result = dict(name=name, image=str(image), started=time.strftime("%Y-%m-%d %H:%M:%S"))


def threads():
    """{thread name: execution cycles} from `kernel thread list`."""
    text = console.collect("kernel thread list", quiet=3.0, timeout=60) or ""
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text).replace("\r", "")
    out, cur = {}, None
    for line in text.splitlines():
        m = re.match(r"\s*\*?\s*0x[0-9a-f]+\s+(\S.*?)\s*$", line)
        if m:
            cur = m[1]
            continue
        m = re.search(r"Total execution cycles:\s*(\d+)\s*\((\d+)\s*%\)", line)
        if m and cur is not None:
            out[cur] = dict(cycles=int(m[1]), percent=int(m[2]))
    return out, text


w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", str(EL), "-w", str(image), "0x90000000", "-v"],
                   capture_output=True, text=True)
result["flash_ok"] = w.returncode == 0
if w.returncode == 0:
    since = console.mark()
    subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
    ready = console.wait_for(r"Matter stack initialized|Matter Server Init failed|Matter init failed", since, 900)
    result["matter_ready"] = ready is not None
    time.sleep(60)
    first, text1 = threads()
    t1 = time.time()
    time.sleep(60)
    second, text2 = threads()
    dt = time.time() - t1
    total = sum(v["cycles"] for v in second.values()) - sum(v["cycles"] for v in first.values())
    delta = {k: second[k]["cycles"] - first.get(k, {}).get("cycles", 0) for k in second}
    result["window_s"] = round(dt, 1)
    result["share_percent"] = {k: round(100 * v / total, 2) for k, v in sorted(delta.items(), key=lambda kv: -kv[1]) if total > 0}
    result["since_boot_percent"] = {k: v["percent"] for k, v in second.items()}
    (OUT / "threads-1.txt").write_text(text1)
    (OUT / "threads-2.txt").write_text(text2)
    mark = console.mark()
    console.send("storage_bench 3 mt/cfg/unique-id")
    console.wait_for(r"val_len \(miss\)", mark, 600)
    time.sleep(1)
    result["bench"] = [t.strip() for _, t in console.lines_since(mark) if re.search(r"us\)|open\+close|read (32|1024)|scan \(|load_one \(|val_len \(", t)]
    idle_since = console.mark()
    time.sleep(30)
    lines = [re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", t).strip() for _, t in console.lines_since(idle_since)]
    result["log_lines_in_30s_idle"] = len(lines)
    result["log_sample"] = lines[:15]
console.close()
raw = OUT / "console.raw"
with open(raw, "rb") as src, gzip.open(OUT / "console.log.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)
raw.unlink()
(OUT / "threads.json").write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
print(json.dumps({k: result.get(k) for k in ("name", "flash_ok", "matter_ready", "window_s", "share_percent", "log_lines_in_30s_idle")}, ensure_ascii=False))
