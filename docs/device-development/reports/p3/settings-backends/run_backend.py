"""Settings backend comparison on the board, one backend per run.

usage: run_backend.py <backend> [cycles]

Owner's method (P3, 2026-09-13):
 1. erase the backend's partition completely
 2. start the backend on the clean partition and let it create its structure
 3. restart the controller
 4. check the backend came up and read back what it created
 5. commissioning cycles - reboot, commission, reboot, remove fabrics - ten
    times, timing Matter's start at every boot, to accumulate deleted records

Everything goes through the board's UART shell (bench_console) and chip-tool;
the only SWD use is flashing the image. Results: <backend>.jsonl (one event per
line) and <backend>.console.raw.
"""
import json, re, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench")
from bench_console import Console

HERE = Path(__file__).parent
BACKEND = sys.argv[1]
CYCLES = int(sys.argv[2]) if len(sys.argv) > 2 else 10
IMAGE = HERE / f"build-{BACKEND}" / "cedar_switch_3in4out_power/zephyr/zephyr.signed.bin"
MATTER = "/Volumes/Programming/Zephyr/zephyr_latest/modules/lib/matter"
CHIP_TOOL = f"{MATTER}/out/chip-tool/chip-tool"
CHIP_STORE = HERE / f"chip-tool-{BACKEND}"
CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
EL = "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"
SN = "002F002B3233510739363634"
MATTER_READY_TIMEOUT = 900
UP = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)")

out = open(HERE / f"{BACKEND}.jsonl", "a")
console = Console(str(HERE / f"{BACKEND}.console.raw"))
time.sleep(1.0)

def record(event, **data):
    data.update(event=event, backend=BACKEND, host_time=time.strftime("%H:%M:%S"))
    line = json.dumps(data)
    print(line, flush=True)
    out.write(line + "\n"); out.flush()

def uptime(text):
    m = UP.search(text)
    return None if not m else int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000

def wait_line(pattern, since, timeout):
    """(host time, line text) of the first matching line after `since`, or None."""
    hit = console.wait_for(pattern, since, timeout)
    return None if hit is None else (hit[0], hit[1].string)

def send_acked(cmd, ack, timeout=15, attempts=3):
    """Send a shell command and wait for its acknowledgement line; resend when the
    console dropped input bytes (P0) and the command never ran. Returns the mark
    taken before the successful attempt, or None."""
    for _ in range(attempts):
        since = console.mark()
        console.send(cmd)
        if wait_line(ack, since, timeout) is not None:
            return since
    return None

def shell(cmd, quiet=2.0, timeout=60):
    return console.collect(cmd, quiet=quiet, timeout=timeout) or ""

def boot(label, how):
    """Reboot and measure the boot up to Matter ready."""
    since = console.mark()
    t0 = time.time()
    if how == "reboot":
        acked = send_acked("kernel reboot cold", r"Starting bootloader|Start main app", timeout=30)
        if acked is not None:
            since = acked
    marks = {}
    hit = wait_line(r"Start main app", since, 120)
    if hit is None:
        record("boot_failed", label=label, stage="main"); return None
    marks["main_host_s"] = round(hit[0] - t0, 2)
    start = wait_line(r"Init CHIP stack", since, 120)
    ready = wait_line(r"Matter stack initialized|Matter Server Init failed|Matter init failed", since, MATTER_READY_TIMEOUT)
    if start is None or ready is None:
        record("boot_failed", label=label, stage="matter", **marks); return None
    s_up, r_up = uptime(start[1]), uptime(ready[1])
    marks.update(matter_start_uptime_s=s_up, matter_ready_uptime_s=r_up,
                 matter_init_s=round(r_up - s_up, 3) if s_up is not None and r_up is not None else None,
                 matter_ok="initialized" in ready[1])
    # A backend error at start shows in the log; keep the lines.
    errs = [t for _, t in console.lines_since(since) if re.search(r"settings|zms|nvs|fcb|littlefs|fs_srv", t, re.I) and re.search(r"<err>|<wrn>", t)]
    marks["storage_warnings"] = errs[:5]
    time.sleep(3)
    global BOOTS
    BOOTS += 1
    record("boot", label=label, boots_since_wipe=BOOTS, **marks)
    return marks

BOOTS = 0  # boots since the partition was erased

def read_setting(name, attempts=4):
    """Hex dump line of a setting, or None. The console drops input bytes now and
    then (P0), so the command's echo is checked and the read retried."""
    cmd = f"settings read {name}"
    for _ in range(attempts):
        text = shell(cmd)
        if cmd not in text:
            continue
        m = re.search(r"00000000: ((?:[0-9a-fA-F]{2} ?)+)", text)
        if m:
            return m.group(1).strip()
    return None

def counter_value(dump):
    """mt/ctr/reboot-count is a little-endian uint32."""
    if not dump:
        return None
    b = [int(x, 16) for x in dump.split()[:4]]
    return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24 if len(b) == 4 else None

def commission(node):
    t0 = time.time()
    since = send_acked("matter commissioning open", r"Matter commissioning window open scheduled")
    if since is None:
        record("commission", node=node, ok=False, window_open_s=None, note="command not accepted"); return False
    opened = wait_line(r"Commissioning window is open|OpenBasicCommissioningWindow failed", since, 300)
    open_s = round(opened[0] - t0, 2) if opened else None
    if not opened or "failed" in opened[1]:
        record("commission", node=node, ok=False, window_open_s=open_s, note="window"); return False
    t1 = time.time()
    log = HERE / f"{BACKEND}.chip-tool-{node}.log"
    with open(log, "w") as f:
        try:
            rc = subprocess.run([CHIP_TOOL, "pairing", "onnetwork-long", str(node), "20202021", "3840",
                                 "--storage-directory", str(CHIP_STORE),
                                 "--paa-trust-store-path", f"{MATTER}/credentials/development/paa-root-certs"],
                                stdout=f, stderr=subprocess.STDOUT, timeout=300).returncode
        except subprocess.TimeoutExpired:
            rc = -1
    ok = rc == 0
    failed_step = None
    if not ok:
        for line in open(log, errors="replace"):
            if "Error on commissioning step" in line or "Failure" in line:
                failed_step = re.sub(r"\x1b\[[0-9;]*m", "", line.strip())[-160:]
    record("commission", node=node, ok=ok, window_open_s=open_s, commission_s=round(time.time() - t1, 1), failed=failed_step)
    return ok

def remove_fabrics():
    t0 = time.time()
    since = send_acked("matter fabric reset", r"Matter fabric reset scheduled")
    done = wait_line(r"Matter fabrics after delete: \d+", since, 600) if since is not None else None
    record("remove_fabrics", ok=done is not None, remove_s=round(done[0] - t0, 2) if done else None,
           after=done[1].split(":")[-1].strip() if done else None)

# -- 0. flash the image ---------------------------------------------------------
CHIP_STORE.mkdir(exist_ok=True)
w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", EL, "-w", str(IMAGE), "0x90000000", "-v"], capture_output=True, text=True)
record("flash", ok=w.returncode == 0)
if w.returncode != 0:
    sys.exit(1)
since = console.mark()
subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
wait_line(r"Start main app", since, 120)
time.sleep(8)

# -- 1. erase the backend's partition ------------------------------------------
# The erase prints nothing until it is done: wait for its result line, not for quiet.
since = console.mark()
t_wipe = time.time()
console.send("storage_wipe yes")
done = wait_line(r"storage_wipe: offset .* erased rc=-?\d+ in \d+ ms", since, 1800)
record("wipe", result=done[1].strip() if done else None, host_s=round(time.time() - t_wipe, 1))
if done is None or "rc=0" not in done[1]:
    sys.exit(2)
time.sleep(1)

# -- 2. start on the clean partition ----------------------------------------------
first = boot("clean partition", "reboot")
counter_1 = read_setting("mt/ctr/reboot-count")
uid_1 = read_setting("mt/cfg/unique-id")
listing = shell("settings list", quiet=3.0)
record("structure_created", reboot_count=counter_value(counter_1), unique_id=uid_1, keys=len([l for l in listing.splitlines() if "/" in l and "settings list" not in l]))

# -- 3/4. restart and check the structure was read back ------------------------------
second = boot("restart after creation", "reboot")
counter_2 = read_setting("mt/ctr/reboot-count")
uid_2 = read_setting("mt/cfg/unique-id")
record("structure_read_back", reboot_count_before=counter_value(counter_1), reboot_count_after=counter_value(counter_2),
       unique_id_same=(uid_1 is not None and uid_1 == uid_2),
       counter_advanced=(counter_value(counter_1) is not None and counter_value(counter_2) == counter_value(counter_1) + 1))

# -- 5. commissioning cycles --------------------------------------------------------
for cycle in range(1, CYCLES + 1):
    boot(f"cycle {cycle}: before commissioning", "reboot")
    commission(100 + cycle)
    boot(f"cycle {cycle}: with fabric", "reboot")
    remove_fabrics()
record("done", cycles=CYCLES)
# The bench prints nothing until it is done: wait for its last line.
since = console.mark()
console.send("storage_bench 3")
wait_line(r"val_len \(miss\)", since, 1800)
time.sleep(1)
bench = "\n".join(t for _, t in console.lines_since(since))
record("bench", text=[l.strip() for l in bench.splitlines() if re.search(r"us\)|open|read|scan|load_one|val_len", l)])
boot("after cycles", "reboot")
final = counter_value(read_setting("mt/ctr/reboot-count"))
record("persistence", reboot_count=final, boots_since_wipe=BOOTS,
       note="Matter counts every boot in mt/ctr/reboot-count; equal counts mean every boot read and wrote it back")
console.close()
