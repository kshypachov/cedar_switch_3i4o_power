"""Storage read time with the network interfaces up and down, without rebuilding.

usage: net_off.py <name> <zephyr.signed.bin>

Owner's question (2026-09-14): does the network driver take CPU time and slow the
file system? Idle thread stats (diag/int-*) show eth_w5500 + rx_q ~1-2 %, but they do
not count time spent in interrupt handlers. `net iface down` on the W5500 calls
w5500_hw_stop(): SIMR = 0 (no chip interrupts) and socket 0 closed, so the chip stops
interrupting and spi2 goes quiet. Stages:
  A  all interfaces up                     storage_bench 3
  B  Ethernet (W5500) down                  storage_bench 3
  C  Ethernet and Wi-Fi (esp_hosted) down   storage_bench 3, kernel thread list
then both brought up again.

Results in tuning/diag/netoff-<name>/: console.log.gz, netoff.json.
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
ROW = re.compile(r"(open\+close|read 32 \+seek|read 32|read 1024|scan \(all\)|load_one \(hit\))\s+(\d+)\s*/\s*(\d+)")

name, image = sys.argv[1], Path(sys.argv[2])
OUT = HERE / f"netoff-{name}"
OUT.mkdir(parents=True, exist_ok=True)
console = Console(str(OUT / "console.raw"))
time.sleep(1.0)
result = dict(name=name, image=str(image), started=time.strftime("%Y-%m-%d %H:%M:%S"))


def clean(text):
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text).replace("\r", "")


def send_echoed(cmd, attempts=4):
    for _ in range(attempts):
        mark = console.mark()
        console.send(cmd)
        if console.wait_for(re.escape(cmd) + r"\s*$", mark, 4) is not None:
            return mark
        time.sleep(1.0)
    return None


def collect(cmd, quiet=2.0):
    mark = send_echoed(cmd)
    if mark is None:
        return ""
    time.sleep(quiet)
    return clean("\n".join(t for _, t in console.lines_since(mark)))


def bench(label):
    mark = send_echoed("storage_bench 3 mt/cfg/unique-id")
    ok = mark is not None and console.wait_for(r"val_len \(miss\)", mark, 600) is not None
    time.sleep(1)
    rows = {}
    if mark is not None:
        for _, t in console.lines_since(mark):
            m = ROW.search(clean(t))
            if m:
                rows[m[1]] = round(int(m[3]) / 1000, 1)
    result[label] = dict(ok=ok, mean_ms=rows)
    print(label, result[label], flush=True)


def interfaces():
    """[(index, name, type line)] from `net iface`."""
    text = collect("net iface", quiet=3.0)
    out = []
    for m in re.finditer(r"Interface (?:(\S+) )?\(0x[0-9a-f]+\) \(([^)]*)\) \[(\d+)\]", text):
        out.append(dict(index=int(m[3]), name=m[1], type=m[2]))
    return out, text


def iface(cmd, idx):
    text = collect(f"net iface {cmd} {idx}", quiet=3.0)
    return text.strip().splitlines()[-3:]


w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", str(EL), "-w", str(image), "0x90000000", "-v"],
                   capture_output=True, text=True)
result["flash_ok"] = w.returncode == 0
if w.returncode == 0:
    since = console.mark()
    subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
    result["matter_ready"] = console.wait_for(r"Matter stack initialized|Matter Server Init failed|Matter init failed", since, 900) is not None
    time.sleep(45)
    ifs, text = interfaces()
    (OUT / "net-iface.txt").write_text(text)
    result["interfaces"] = ifs
    eth = next((i for i in ifs if "ETHERNET" in i["type"].upper()), None)
    wifi = next((i for i in ifs if "WIFI" in i["type"].upper() or (i["name"] or "").startswith("wlan")), None)
    bench("A_all_up")
    if eth:
        result["eth_down"] = iface("down", eth["index"])
        time.sleep(10)
        bench("B_eth_down")
    if wifi:
        result["wifi_down"] = iface("down", wifi["index"])
        time.sleep(10)
        bench("C_eth_wifi_down")
    (OUT / "threads-down.txt").write_text(collect("kernel thread list", quiet=3.0))
    for i in (eth, wifi):
        if i:
            result.setdefault("up_again", []).append(iface("up", i["index"]))
    time.sleep(10)
    bench("D_all_up_again")
console.close()
raw = OUT / "console.raw"
with open(raw, "rb") as src, gzip.open(OUT / "console.log.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)
raw.unlink()
(OUT / "netoff.json").write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
print(json.dumps({k: result.get(k) for k in ("name", "interfaces", "A_all_up", "B_eth_down", "C_eth_wifi_down", "D_all_up_again")},
                 ensure_ascii=False))
