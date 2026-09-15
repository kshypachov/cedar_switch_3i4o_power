"""Five-fabric check of one tuning candidate on the board.

usage: run_fabrics.py --backend zms --name comboA-sectors4 --image <zephyr.signed.bin>
                      --work <scratch dir> [--rounds 2] [--files <conf> <overlay> ...]

Method (frozen in tuning/README.md before the first run):
 1. flash, erase the backend's partition, start on it, reboot and check the read-back
 2. <rounds> times: commission five fabrics one after another (commissioners
    alpha, beta, gamma, 4, 5; nodes r01..r05), reboot after each and count the
    fabrics the device really has; storage_bench at five fabrics; delete all
    fabrics (matter fabric reset, repeated until the count is 0); reboot
 3. persistence: Matter's reboot counter must equal the boots since the erase

Results in tuning/fabrics/<backend>-<name>/: run.jsonl, console.log.gz,
chip-tool.log.gz, image.sha256, meta.json, copies of the conf/overlay files,
then metrics.json and fabrics/results.md from summarize.py.

Shell and reboot handling are those of run_trial.py (echo-checked commands,
reboots acknowledged by the bootloader line so a boot is never doubled).
"""
import argparse, gzip, hashlib, json, re, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[5]
WEST = REPO.parent
sys.path.insert(0, str(REPO / "tests/bench"))
from bench_console import Console  # noqa: E402

MATTER = WEST / "modules/lib/matter"
CHIP_TOOL = MATTER / "out/chip-tool/chip-tool"
CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
EL = REPO / "CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"
SN = "002F002B3233510739363634"
MATTER_READY_TIMEOUT = 1800
REMOVE_TIMEOUT = 1800
BENCH_KEY = "mt/cfg/unique-id"
COMMISSIONERS = ["alpha", "beta", "gamma", "4", "5"]
UP = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)")
# Commissioning phases, device uptime between the first of each line after the
# window command (as tuning/analyze.py).
MARKS = [("pase_start", "Commissioning session establishment step started"),
         ("pase_done", "Commissioning completed session establishment step"), ("failsafe", "Received ArmFailSafe"),
         ("attest", "AttestationRequest successful"), ("csr", "CSRRequest successful"),
         ("complete", "Received CommissioningComplete"), ("commit", "was committed to storage")]
PHASES = [("pase", "pase_start", "pase_done"), ("attestation", "failsafe", "attest"), ("csr", "attest", "csr"),
          ("noc_to_done", "csr", "complete"), ("commit", "complete", "commit")]

ap = argparse.ArgumentParser()
ap.add_argument("--backend", required=True)
ap.add_argument("--name", required=True)
ap.add_argument("--image", required=True, type=Path)
ap.add_argument("--work", required=True, type=Path)
ap.add_argument("--rounds", type=int, default=2)
ap.add_argument("--change", default="")
ap.add_argument("--files", nargs="*", default=[])
# Board B (2026-09-15): its image goes in through MCUboot CDC recovery beforehand (P6
# run_image_b.py), so no ST-Link flashing here; the console is found by that board's USB serial.
ap.add_argument("--no-flash", action="store_true", help="image already on the board: start with a shell reboot")
ap.add_argument("--console-serial", default=None, help="USB serial of the console (default: board A's ST-LINK)")
args = ap.parse_args()

OUT = HERE / f"{args.backend}-{args.name}"
if OUT.exists() and any(OUT.iterdir()):
    sys.exit(f"{OUT} is not empty")
OUT.mkdir(parents=True, exist_ok=True)
CHIP_STORE = args.work / f"chip-tool-fabrics-{args.backend}-{args.name}"
CHIP_STORE.mkdir(parents=True, exist_ok=True)

meta = dict(backend=args.backend, name=args.name, change=args.change, rounds=args.rounds,
            image=str(args.image), started=time.strftime("%Y-%m-%d %H:%M:%S"))
(OUT / "meta.json").write_text(json.dumps(meta, indent=1, ensure_ascii=False) + "\n")
(OUT / "image.sha256").write_text(hashlib.sha256(args.image.read_bytes()).hexdigest() + "  zephyr.signed.bin\n")
for f in args.files:
    shutil.copy(f, OUT / Path(f).name)

out = open(OUT / "run.jsonl", "a")
console = Console(str(OUT / "console.raw"), **({"usb_serial": args.console_serial} if args.console_serial else {}))
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
    subprocess.run([sys.executable, str(HERE / "summarize.py"), str(OUT)])
    sys.exit(code)


def uptime(text):
    m = UP.search(text)
    return None if not m else int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000


def wait_line(pattern, since, timeout):
    hit = console.wait_for(pattern, since, timeout)
    return None if hit is None else (hit[0], hit[1].string)


def send_echoed(cmd, attempts=4, echo_timeout=4):
    for _ in range(attempts):
        since = console.mark()
        console.send(cmd)
        if wait_line(re.escape(cmd) + r"\s*$", since, echo_timeout) is not None:
            return since
        time.sleep(1.0)
    return None


def send_acked(cmd, ack, timeout=15, attempts=3):
    for _ in range(attempts):
        since = console.mark()
        console.send(cmd)
        if wait_line(ack, since, timeout) is not None:
            return since
    return None


def sent_at(since, cmd):
    hit = wait_line(re.escape(cmd), since, 0)
    return hit[0] if hit else None


def shell(cmd, quiet=2.0, timeout=60):
    return console.collect(cmd, quiet=quiet, timeout=timeout) or ""


def fabric_count(attempts=4):
    for _ in range(attempts):
        m = re.search(r"Matter fabric count: (\d+)", shell("matter fabric count"))
        if m:
            return int(m[1])
    return None


def boot(label, **extra):
    """Reboot, measure the boot up to Matter ready, count the fabrics."""
    global BOOTS
    t0 = time.time()
    since = None
    for _ in range(4):
        mark = console.mark()
        console.send("kernel reboot cold")
        if wait_line(r"Starting bootloader", mark, 15) is not None:
            since = mark
            break
    if since is None:
        record("boot_failed", label=label, stage="no bootloader after reboot command", **extra)
        return None
    marks = {}
    hit = wait_line(r"Start main app", since, 120)
    if hit is None:
        record("boot_failed", label=label, stage="main", **extra)
        return None
    marks["main_host_s"] = round(hit[0] - t0, 2)
    start = wait_line(r"Init CHIP stack", since, 120)
    ready = wait_line(r"Matter stack initialized|Matter Server Init failed|Matter init failed", since, MATTER_READY_TIMEOUT)
    if start is None or ready is None:
        record("boot_failed", label=label, stage="matter", **extra, **marks)
        return None
    s_up, r_up = uptime(start[1]), uptime(ready[1])
    marks.update(matter_init_s=round(r_up - s_up, 3) if s_up is not None and r_up is not None else None,
                 matter_ok="initialized" in ready[1])
    lines = [t for _, t in console.lines_since(since)]
    marks["zms"] = [re.sub(r".*fs_zms: ", "", t).strip() for t in lines
                    if re.search(r"fs_zms: (alloc wra|data wra|\d+ Sectors|.*[Gg][Cc])", t)]
    marks["storage_warnings"] = [t.strip()[-160:] for t in lines
                                 if re.search(r"settings|zms|littlefs|fs_srv|spi_nor|flash|fs:", t, re.I)
                                 and re.search(r"<err>|<wrn>", t)][:5]
    time.sleep(3)
    BOOTS += 1
    marks["fabrics"] = fabric_count()
    record("boot", label=label, boots_since_wipe=BOOTS, **extra, **marks)
    return marks


def read_setting(name, attempts=4):
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
    if not dump:
        return None
    b = [int(x, 16) for x in dump.split()[:4]]
    return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24 if len(b) == 4 else None


def device_phases(since):
    first = {}
    for _, line in console.lines_since(since):
        t = uptime(line)
        if t is None:
            continue
        for key, needle in MARKS:
            if needle in line and key not in first:
                first[key] = t
    return {p: (round(first[z] - first[a], 3) if a in first and z in first and first[z] >= first[a] else None)
            for p, a, z in PHASES}


def commission(rnd, k, node, commissioner):
    t0 = time.time()
    since = send_acked("matter commissioning open", r"Matter commissioning window open scheduled")
    base = dict(round=rnd, k=k, node=node, commissioner=commissioner)
    if since is None:
        record("commission", ok=False, note="command not accepted", **base)
        return False
    opened = wait_line(r"Commissioning window is open|Commissioning window is already open|OpenBasicCommissioningWindow failed",
                       since, 600)
    t0 = sent_at(since, "matter commissioning open") or t0
    open_s = round(opened[0] - t0, 2) if opened else None
    if not opened or "failed" in opened[1]:
        record("commission", ok=False, window_open_s=open_s, note="window", **base)
        return False
    t1 = time.time()
    log = OUT / f"chip-tool-{node}.log"
    with open(log, "w") as f:
        try:
            rc = subprocess.run([str(CHIP_TOOL), "pairing", "onnetwork-long", str(node), "20202021", "3840",
                                 "--storage-directory", str(CHIP_STORE),
                                 "--paa-trust-store-path", str(MATTER / "credentials/development/paa-root-certs"),
                                 "--commissioner-name", commissioner],
                                stdout=f, stderr=subprocess.STDOUT, timeout=600).returncode
        except subprocess.TimeoutExpired:
            rc = -1
    ok = rc == 0
    failed_step = None
    if not ok:
        for line in open(log, errors="replace"):
            if "Error on commissioning step" in line or "Failure" in line:
                failed_step = re.sub(r"\x1b\[[0-9;]*m", "", line.strip())[-160:]
    time.sleep(2)
    device_errors = [t.strip()[-160:] for _, t in console.lines_since(since) if "<err>" in t][:8]
    record("commission", ok=ok, window_open_s=open_s, commission_s=round(time.time() - t1, 1), failed=failed_step,
           phases=device_phases(since), device_errors=device_errors, **base)
    return ok


def remove_fabrics(rnd):
    for attempt in range(1, 4):
        t0 = time.time()
        since = send_acked("matter fabric reset", r"Matter fabric reset scheduled")
        done = wait_line(r"Matter fabrics after delete: \d+", since, REMOVE_TIMEOUT) if since is not None else None
        if since is not None:
            t0 = sent_at(since, "matter fabric reset") or t0
        before = wait_line(r"Deleting all Matter fabrics, count=\d+", since, 0) if since is not None else None
        after = int(re.search(r"after delete: (\d+)", done[1])[1]) if done else None
        record("remove_fabrics", round=rnd, attempt=attempt, ok=done is not None,
               remove_s=round(done[0] - t0, 2) if done else None,
               before=int(re.search(r"count=(\d+)", before[1])[1]) if before else None, after=after)
        if after == 0:
            return
        time.sleep(3)


def bench(label):
    since = send_echoed(f"storage_bench 3 {BENCH_KEY}")
    end = wait_line(r"val_len \(miss\)", since, 1800) if since is not None else None
    time.sleep(1)
    lines = [t.strip() for _, t in console.lines_since(since)] if since is not None else []
    record("bench", label=label, ok=end is not None, key=BENCH_KEY,
           text=[l for l in lines if re.search(r"us\)|open\+close|read (32|1024)|scan \(|load_one \(|val_len \(", l)])


# -- 0. flash ------------------------------------------------------------------------
if args.no_flash:
    record("flash", skipped=True, note="image put on the board beforehand")
    since = None
    for _ in range(4):
        mark = console.mark()
        console.send("kernel reboot cold")
        if wait_line(r"Starting bootloader|Booting MCUboot|Start main app", mark, 20) is not None:
            since = mark
            break
    if since is None or wait_line(r"Start main app", since, 120) is None:
        finish(1, "no boot after the shell reboot")
else:
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
wait_line(r"Matter stack initialized|Matter Server Init failed|Matter init failed", since, 900)
time.sleep(3)

# -- 1. erase, start, read back ----------------------------------------------------------
t_wipe = time.time()
since = send_echoed("storage_wipe yes")
done = wait_line(r"storage_wipe: offset .* erased rc=-?\d+ in \d+ ms", since, 1800) if since is not None else None
record("wipe", result=done[1].strip() if done else None, host_s=round(time.time() - t_wipe, 1))
if done is None or "rc=0" not in done[1]:
    finish(2, "wipe failed")
time.sleep(1)
if boot("clean partition") is None:
    finish(3, "boot failed")
counter_1 = counter_value(read_setting("mt/ctr/reboot-count"))
uid_1 = read_setting("mt/cfg/unique-id")
if boot("restart after creation") is None:
    finish(3, "boot failed")
counter_2 = counter_value(read_setting("mt/ctr/reboot-count"))
uid_2 = read_setting("mt/cfg/unique-id")
record("structure_read_back", reboot_count_before=counter_1, reboot_count_after=counter_2,
       unique_id_same=(uid_1 is not None and uid_1 == uid_2),
       counter_advanced=(counter_1 is not None and counter_2 == counter_1 + 1))

# -- 2. rounds of five fabrics ---------------------------------------------------------------
for rnd in range(1, args.rounds + 1):
    for k, commissioner in enumerate(COMMISSIONERS, 1):
        commission(rnd, k, rnd * 100 + k, commissioner)
        if boot(f"round {rnd}: after commissioning {k}", round=rnd, k=k) is None:
            finish(3, "boot failed")
    bench(f"round {rnd}: five fabrics")
    remove_fabrics(rnd)
    if boot(f"round {rnd}: after removal", round=rnd, k=0) is None:
        finish(3, "boot failed")

# -- 3. persistence -----------------------------------------------------------------------
final = counter_value(read_setting("mt/ctr/reboot-count"))
record("persistence", reboot_count=final, boots_since_wipe=BOOTS, ok=final == BOOTS)
finish(0)
