"""ICACHE hit/miss counters around a storage read, without rebuilding.

usage: icache_monitor.py <name> <zephyr.signed.bin>

Hypothesis (2026-09-14, file/039): the app executes from external flash through the
STM32U5 ICACHE remap 0x0200_0000 -> 0x9000_0000 (sysbuild/mcuboot_hooks/boot-hook.c),
in 1-way direct-mapped mode (CONFIG_CACHE_STM32_ICACHE_DIRECT_MAPPING=y). The images
built on 09-14 have all code after eth_w5500 shifted by 0x78, and File + SPI interrupt
reads twice as slowly. A layout-dependent cache collision would show as more ICACHE
misses for the same storage_bench on the new image.

ICACHE (non-secure) at 0x40030400: CR +0x00 (EN 0, WAYSEL 2, HITMEN 16, MISSMEN 17,
HITMRST 18, MISSMRST 19), HMONR +0x10 (32 bit), MMONR +0x14 (16 bit, saturates at 0xFFFF).
Monitors are enabled with the cache running (allowed; WAYSEL writes are ignored while EN=1).

Results in tuning/diag/<name>/: console.log.gz, icache.json.
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
CR, HMONR, MMONR = 0x40030400, 0x40030410, 0x40030414
MON_EN, MON_RST = (1 << 16) | (1 << 17), (1 << 18) | (1 << 19)

name, image = sys.argv[1], Path(sys.argv[2])
OUT = HERE / f"icache-{name}"
OUT.mkdir(parents=True, exist_ok=True)
console = Console(str(OUT / "console.raw"))
time.sleep(1.0)
result = dict(name=name, image=str(image), started=time.strftime("%Y-%m-%d %H:%M:%S"))


def read(addr, attempts=4):
    for _ in range(attempts):
        text = console.collect(f"devmem {addr:x} 32", quiet=1.5, timeout=20) or ""
        m = re.search(r"Read value (0x[0-9a-fA-F]+)", text)
        if m:
            return int(m[1], 16)
    return None


def write(addr, value):
    console.collect(f"devmem {addr:x} 32 0x{value:x}", quiet=1.5, timeout=20)


def counters(label, action):
    """Reset and enable the monitors, run `action`, read hits and misses."""
    cr = read(CR)
    write(CR, cr | MON_RST)
    write(CR, (cr & ~MON_RST) | MON_EN)
    t0 = time.time()
    action()
    hits, misses = read(HMONR), read(MMONR)
    entry = dict(cr=None if cr is None else hex(cr), hits=hits, misses=misses, seconds=round(time.time() - t0, 1),
                 miss_saturated=misses == 0xFFFF,
                 miss_percent=round(100 * misses / (hits + misses), 3) if hits is not None and misses is not None and hits + misses else None)
    result[label] = entry
    print(label, entry, flush=True)


def idle():
    time.sleep(10)


def bench():
    # Echo-checked: the console drops input bytes now and then ("storagebench: command
    # not found" in diag/base-0914), and an unsent bench would wait out the timeout.
    cmd = "storage_bench 1 mt/cfg/unique-id"
    for _ in range(4):
        mark = console.mark()
        console.send(cmd)
        if console.wait_for(re.escape(cmd) + r"\s*$", mark, 4) is not None:
            break
        time.sleep(1.0)
    console.wait_for(r"val_len \(miss\)", mark, 600)
    time.sleep(1)
    result["bench"] = [t.strip() for _, t in console.lines_since(mark) if re.search(r"us\)|open\+close|read (32|1024)|scan \(|load_one \(|val_len \(", t)]


w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", str(EL), "-w", str(image), "0x90000000", "-v"],
                   capture_output=True, text=True)
result["flash_ok"] = w.returncode == 0
if w.returncode == 0:
    since = console.mark()
    subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
    result["matter_ready"] = console.wait_for(r"Matter stack initialized|Matter Server Init failed|Matter init failed", since, 900) is not None
    time.sleep(30)
    cr = read(CR)
    result["cr_at_start"] = None if cr is None else hex(cr)
    result["way_mode"] = None if cr is None else ("2-way" if cr & (1 << 2) else "1-way (direct)")
    counters("idle_10s", idle)
    counters("storage_bench_1", bench)
    counters("idle_10s_again", idle)
console.close()
raw = OUT / "console.raw"
with open(raw, "rb") as src, gzip.open(OUT / "console.log.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)
raw.unlink()
(OUT / "icache.json").write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
print(json.dumps({k: result.get(k) for k in ("name", "way_mode", "idle_10s", "storage_bench_1")}, ensure_ascii=False))
