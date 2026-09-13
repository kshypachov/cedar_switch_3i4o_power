"""Flash each LittleFS variant, capture its boot until Matter is ready, run storage_bench.

usage: compare.py <variant> [<variant> ...]   (variants are build-<name> dirs next to this file)
Writes <name>.boot.raw, <name>.bench.txt and a summary line per variant to summary.txt.
"""
import re, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench")
from bench_console import Console

HERE = Path(__file__).parent
CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
EL = "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"
SN = "002F002B3233510739363634"
UP = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)")
MARKS = [("stack start", "Init CHIP stack"), ("fabric 1", "Fabric index 0x1 was retrieved"),
         ("fabric 2", "Fabric index 0x2 was retrieved"), ("acl loaded", "DefaultAclStorage: "),
         ("ready", "Matter stack initialized")]

def uptime(line):
    m = UP.search(line)
    return None if not m else int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000

summary = open(HERE / "summary.txt", "a")
for name in sys.argv[1:]:
    image = HERE / f"build-{name}" / "cedar_switch_3in4out_power/zephyr/zephyr.signed.bin"
    c = Console(str(HERE / f"{name}.boot.raw"))
    time.sleep(1.0)
    w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", EL, "-w", str(image), "0x90000000", "-v"], capture_output=True, text=True)
    if w.returncode != 0:
        print(name, "flash failed"); c.close(); continue
    mark = c.mark()
    subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
    found = {}
    deadline = time.time() + 300
    while time.time() < deadline and "ready" not in found:
        time.sleep(2)
        for _, text in c.lines_since(mark):
            for key, needle in MARKS:
                if key not in found and needle in text and uptime(text) is not None:
                    found[key] = uptime(text)
    time.sleep(5)
    bench = c.collect("storage_bench 5", quiet=3.0, timeout=240) or ""
    c.close()
    (HERE / f"{name}.bench.txt").write_text(bench)
    marks = " ".join(f"{k}={found[k]:.1f}s" for k, _ in MARKS if k in found)
    lines = [l.strip() for l in bench.splitlines() if re.search(r"open\+close|read 32|read 1024|scan \(|bytes,", l)]
    text = f"{name}: {marks}\n  " + "\n  ".join(lines)
    print(text, flush=True); summary.write(text + "\n"); summary.flush()
