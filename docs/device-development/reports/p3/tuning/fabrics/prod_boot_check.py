"""Boot check of the production image after the switch to ZMS (2026-09-14).

usage: prod_boot_check.py <name> <zephyr.signed.bin>

Flashes the image as it ships (no extra conf), resets, waits for Matter and
records what the settings backend reports at start: ZMS mount ("4 Sectors of
4096 bytes", "alloc wra"), the settings partition, storage errors and whether
Matter came up. Then a second reboot through the shell to see the store read
back. No wipe: the partition keeps what the previous run left.

Results in tuning/fabrics/<name>/: console.log.gz, boot.json.
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
STORAGE = re.compile(r"settings|zms|littlefs|fs_srv|spi_nor|flash|\bfs:", re.I)

name, image = sys.argv[1], Path(sys.argv[2])
OUT = HERE / name
OUT.mkdir(parents=True, exist_ok=True)
console = Console(str(OUT / "console.raw"))
time.sleep(1.0)
result = dict(name=name, image=str(image), started=time.strftime("%Y-%m-%d %H:%M:%S"))


def clean(t):
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", t).replace("\r", "").strip()


def boot_record(label, since):
    ready = console.wait_for(r"Matter stack initialized|Matter Server Init failed|Matter init failed", since, 600)
    time.sleep(3)
    lines = [clean(t) for _, t in console.lines_since(since)]
    result[label] = dict(
        matter=None if ready is None else clean(ready[1].string)[-60:],
        zms=[re.sub(r".*fs_zms: ", "", l) for l in lines if "fs_zms:" in l],
        storage_err=[l[-160:] for l in lines if STORAGE.search(l) and re.search(r"<err>|<wrn>", l)][:8],
        fabric_lines=[l[-120:] for l in lines if re.search(r"fabric", l, re.I) and "<inf>" in l][:6])
    print(label, json.dumps(result[label], ensure_ascii=False), flush=True)


w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", str(EL), "-w", str(image), "0x90000000", "-v"],
                   capture_output=True, text=True)
result["flash_ok"] = w.returncode == 0
if w.returncode == 0:
    since = console.mark()
    subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
    boot_record("boot_after_flash", since)
    for _ in range(4):
        mark = console.mark()
        console.send("kernel reboot cold")
        if console.wait_for(r"Starting bootloader", mark, 15) is not None:
            break
    boot_record("boot_after_reboot", mark)
console.close()
raw = OUT / "console.raw"
with open(raw, "rb") as src, gzip.open(OUT / "console.log.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)
raw.unlink()
(OUT / "boot.json").write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
