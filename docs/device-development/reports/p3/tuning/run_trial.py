"""One settings-backend tuning trial on the board.

usage: run_trial.py --backend zms --trial 004-zms_lookup_cache_size-1024 --image <zephyr.signed.bin>
                    --work <scratch dir> [--cycles 5] [--kind trial] [--change "CONFIG_...=..."]
                    [--files <trial.conf> <trial.overlay> ...]

The owner's method (P3 settings-backends/run_backend.py, same steps):
 1. flash the image, erase the backend's whole partition (storage_wipe yes)
 2. start on the clean partition and let the backend create its structure
 3. reboot, check the structure was read back (mt/ctr/reboot-count +1, same unique-id)
 4. <cycles> times: reboot, commission with chip-tool, reboot, remove fabrics
 5. storage_bench 3 mt/cfg/unique-id, a last reboot and the persistence check
    (Matter's reboot counter must equal the boots since the erase)

Results in tuning/<backend>/<trial>/: trial.jsonl (one event per line),
console.log.gz, chip-tool.log.gz, image.sha256, meta.json, copies of the
trial's conf/overlay files, then metrics.json from analyze.py and a
regenerated results.md.

Differences from run_backend.py, each for a failure seen in P3 data:
 - every shell command waits for its echo and is resent when the console
   dropped a byte ("kernel reboot cod" in zms.console.log); reboots are never
   resent once echoed, so a boot cannot be counted twice;
 - storage_wipe/storage_bench are sent the same way before waiting for their
   last line (they print nothing until done);
 - a fresh chip-tool storage directory per trial;
 - the bench hit key is one Matter writes on every erase-and-start.
"""
import argparse, gzip, hashlib, json, re, shutil, subprocess, sys, time
from pathlib import Path

TUNING = Path(__file__).resolve().parent
REPO = TUNING.parents[4]
WEST = REPO.parent
sys.path.insert(0, str(REPO / "tests/bench"))
from bench_console import Console  # noqa: E402

MATTER = WEST / "modules/lib/matter"
CHIP_TOOL = MATTER / "out/chip-tool/chip-tool"
CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
EL = REPO / "CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"
SN = "002F002B3233510739363634"
MATTER_READY_TIMEOUT = 900
BENCH_KEY = "mt/cfg/unique-id"
UP = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)")

ap = argparse.ArgumentParser()
ap.add_argument("--backend", required=True)
ap.add_argument("--trial", required=True)
ap.add_argument("--image", required=True, type=Path)
ap.add_argument("--work", required=True, type=Path)
ap.add_argument("--cycles", type=int, default=5)
ap.add_argument("--kind", default="trial", choices=["base", "trial", "combo", "confirm"])
ap.add_argument("--change", default="")
ap.add_argument("--files", nargs="*", default=[])
args = ap.parse_args()

OUT = TUNING / args.backend / args.trial
if OUT.exists() and any(OUT.iterdir()):
    sys.exit(f"{OUT} is not empty")
OUT.mkdir(parents=True, exist_ok=True)
CHIP_STORE = args.work / f"chip-tool-{args.backend}-{args.trial}"
CHIP_STORE.mkdir(parents=True, exist_ok=True)

meta = dict(backend=args.backend, trial=args.trial, kind=args.kind, change=args.change, cycles=args.cycles,
            image=str(args.image), started=time.strftime("%Y-%m-%d %H:%M:%S"))
(OUT / "meta.json").write_text(json.dumps(meta, indent=1, ensure_ascii=False) + "\n")
(OUT / "image.sha256").write_text(hashlib.sha256(args.image.read_bytes()).hexdigest() + "  zephyr.signed.bin\n")
for f in args.files:
    shutil.copy(f, OUT / Path(f).name)

out = open(OUT / "trial.jsonl", "a")
console = Console(str(OUT / "console.raw"))
time.sleep(1.0)
BOOTS = 0  # boots since the partition was erased


def record(event, **data):
    data.update(event=event, host_time=time.strftime("%H:%M:%S"))
    line = json.dumps(data, ensure_ascii=False)
    print(line, flush=True)
    out.write(line + "\n")
    out.flush()


def finish(code, reason=None):
    if reason:
        record("aborted", reason=reason)
    console.close()
    raw = OUT / "console.raw"
    with open(raw, "rb") as src, gzip.open(OUT / "console.log.gz", "wb") as dst:
        shutil.copyfileobj(src, dst)
    raw.unlink()
    logs = sorted(OUT.glob("chip-tool-*.log"))
    if logs:
        with gzip.open(OUT / "chip-tool.log.gz", "wb") as dst:
            for log in logs:
                dst.write(f"===== {log.name}\n".encode())
                dst.write(log.read_bytes())
                log.unlink()
    subprocess.run([sys.executable, str(TUNING / "analyze.py"), str(OUT)])
    subprocess.run([sys.executable, str(TUNING / "results.py"), args.backend])
    sys.exit(code)


def uptime(text):
    m = UP.search(text)
    return None if not m else int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000


def wait_line(pattern, since, timeout):
    """(host time, line text) of the first matching line after `since`, or None."""
    hit = console.wait_for(pattern, since, timeout)
    return None if hit is None else (hit[0], hit[1].string)


def send_echoed(cmd, attempts=4, echo_timeout=4):
    """Send a shell command until the shell echoes it whole. Returns the mark taken
    before the echoed attempt, or None. The console drops input bytes now and
    then (P0): a mangled command shows as a mangled echo and never ran as sent."""
    for _ in range(attempts):
        since = console.mark()
        console.send(cmd)
        if wait_line(re.escape(cmd) + r"\s*$", since, echo_timeout) is not None:
            return since
        time.sleep(1.0)
    return None


def send_acked(cmd, ack, timeout=15, attempts=3):
    """Send and wait for the command's own acknowledgement line; resend if absent."""
    for _ in range(attempts):
        since = console.mark()
        console.send(cmd)
        if wait_line(ack, since, timeout) is not None:
            return since
    return None


def sent_at(since, cmd):
    """Host time of the echo of `cmd` after `since`: the start of the attempt that
    ran, so a resend after a dropped byte does not count as the command's time."""
    hit = wait_line(re.escape(cmd), since, 0)
    return hit[0] if hit else None


def shell(cmd, quiet=2.0, timeout=60):
    return console.collect(cmd, quiet=quiet, timeout=timeout) or ""


def boot(label):
    """Reboot and measure the boot up to Matter ready."""
    global BOOTS
    t0 = time.time()
    # Acknowledged by the bootloader, not by the echo: the board often resets
    # before the echo line is complete ("kernel reboot c<reset>"), and a resend
    # then reboots it a second time (zms/004b). Resent only when no boot starts.
    since = None
    for _ in range(4):
        mark = console.mark()
        console.send("kernel reboot cold")
        if wait_line(r"Starting bootloader", mark, 15) is not None:
            since = mark
            break
    if since is None:
        record("boot_failed", label=label, stage="no bootloader after reboot command")
        return None
    marks = {}
    hit = wait_line(r"Start main app", since, 120)
    if hit is None:
        record("boot_failed", label=label, stage="main")
        return None
    marks["main_host_s"] = round(hit[0] - t0, 2)
    start = wait_line(r"Init CHIP stack", since, 120)
    ready = wait_line(r"Matter stack initialized|Matter Server Init failed|Matter init failed", since, MATTER_READY_TIMEOUT)
    if start is None or ready is None:
        record("boot_failed", label=label, stage="matter", **marks)
        return None
    s_up, r_up = uptime(start[1]), uptime(ready[1])
    marks.update(matter_start_uptime_s=s_up, matter_ready_uptime_s=r_up,
                 matter_init_s=round(r_up - s_up, 3) if s_up is not None and r_up is not None else None,
                 matter_ok="initialized" in ready[1])
    errs = [t for _, t in console.lines_since(since)
            if re.search(r"settings|zms|littlefs|fs_srv|spi_nor|flash", t, re.I) and re.search(r"<err>|<wrn>", t)]
    marks["storage_warnings"] = errs[:5]
    time.sleep(3)
    BOOTS += 1
    record("boot", label=label, boots_since_wipe=BOOTS, **marks)
    return marks


def read_setting(name, attempts=4):
    """Hex bytes of a setting, or None; the echo is checked and the read retried."""
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
        record("commission", node=node, ok=False, window_open_s=None, note="command not accepted")
        return False
    opened = wait_line(r"Commissioning window is open|OpenBasicCommissioningWindow failed", since, 300)
    t0 = sent_at(since, "matter commissioning open") or t0
    open_s = round(opened[0] - t0, 2) if opened else None
    if not opened or "failed" in opened[1]:
        record("commission", node=node, ok=False, window_open_s=open_s, note="window")
        return False
    t1 = time.time()
    log = OUT / f"chip-tool-{node}.log"
    with open(log, "w") as f:
        try:
            rc = subprocess.run([str(CHIP_TOOL), "pairing", "onnetwork-long", str(node), "20202021", "3840",
                                 "--storage-directory", str(CHIP_STORE),
                                 "--paa-trust-store-path", str(MATTER / "credentials/development/paa-root-certs")],
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
    if since is not None:
        t0 = sent_at(since, "matter fabric reset") or t0
    record("remove_fabrics", ok=done is not None, remove_s=round(done[0] - t0, 2) if done else None,
           after=done[1].split(":")[-1].strip() if done else None)


# -- 0. flash the image ---------------------------------------------------------
for attempt in range(2):
    w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", str(EL), "-w", str(args.image), "0x90000000", "-v"],
                       capture_output=True, text=True)
    if w.returncode == 0:
        break
record("flash", ok=w.returncode == 0, attempts=attempt + 1)
if w.returncode != 0:
    (OUT / "flash.log").write_text(w.stdout + w.stderr)
    finish(1, "flash failed")
since = console.mark()
subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
if wait_line(r"Start main app", since, 120) is None:
    finish(1, "no boot after flashing")
# The previous trial's data may be in a format this image reads slowly or not at
# all; let the start finish (or give up) before erasing under it.
wait_line(r"Matter stack initialized|Matter Server Init failed|Matter init failed", since, 600)
time.sleep(3)

# -- 1. erase the backend's partition ------------------------------------------
t_wipe = time.time()
since = send_echoed("storage_wipe yes")
done = wait_line(r"storage_wipe: offset .* erased rc=-?\d+ in \d+ ms", since, 1800) if since is not None else None
record("wipe", result=done[1].strip() if done else None, host_s=round(time.time() - t_wipe, 1))
if done is None or "rc=0" not in done[1]:
    finish(2, "wipe failed")
time.sleep(1)

# -- 2. start on the clean partition --------------------------------------------
boot("clean partition")
counter_1 = counter_value(read_setting("mt/ctr/reboot-count"))
uid_1 = read_setting("mt/cfg/unique-id")
record("structure_created", reboot_count=counter_1, unique_id=uid_1)

# -- 3. restart and check the structure was read back ----------------------------
boot("restart after creation")
counter_2 = counter_value(read_setting("mt/ctr/reboot-count"))
uid_2 = read_setting("mt/cfg/unique-id")
record("structure_read_back", reboot_count_before=counter_1, reboot_count_after=counter_2,
       unique_id_same=(uid_1 is not None and uid_1 == uid_2),
       counter_advanced=(counter_1 is not None and counter_2 == counter_1 + 1))

# -- 4. commissioning cycles ------------------------------------------------------
for cycle in range(1, args.cycles + 1):
    boot(f"cycle {cycle}: before commissioning")
    commission(100 + cycle)
    boot(f"cycle {cycle}: with fabric")
    remove_fabrics()
record("cycles_done", cycles=args.cycles)

# -- 5. bench and persistence --------------------------------------------------------
since = send_echoed(f"storage_bench 3 {BENCH_KEY}")
end = wait_line(r"val_len \(miss\)", since, 1800) if since is not None else None
time.sleep(1)
lines = [t.strip() for _, t in console.lines_since(since)] if since is not None else []
record("bench", ok=end is not None, key=BENCH_KEY,
       text=[l for l in lines if re.search(r"us\)|open\+close|read (32|1024)|scan \(|load_one \(|val_len \(", l)])
boot("after cycles")
final = counter_value(read_setting("mt/ctr/reboot-count"))
record("persistence", reboot_count=final, boots_since_wipe=BOOTS, ok=final == BOOTS,
       note="Matter counts every boot in mt/ctr/reboot-count; equal counts mean every boot read and wrote it back")
finish(0)
