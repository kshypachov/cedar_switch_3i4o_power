#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A series of warm or cold starts, each one measured rather than watched.

Warm starts are driven through the debugger (`STM32_Programmer_CLI -hardRst`,
the NRST line). Cold starts need the supply removed, which nothing on the
bench can do, so in `--cold` mode the script waits for a person to cycle the
power and treats the MCUboot banner as the moment the board came back. Boot
reason cannot be read back from the board to tell the two apart: Matter's
DiagnosticDataProvider reads and clears the RCC reset flags during init and
counts a pin reset as a power-on reboot, so the mode is recorded by the script.

For every start it records, from the console and from the network:

- time from the MCUboot banner to handover, Zephyr boot, `Start main app`,
  PSRAM self-test, Ethernet up and the ESP-Hosted handshake;
- time until HTTP answers (`GET /api/relays/state`);
- DHCP lease and carrier from `net iface`, and whether `wifi scan` works,
  which is the only cheap proof that the C6 transport is really up;
- what `/api/relays/state` reports and the output register bits of the four
  relay pins, so a relay that is logically off but electrically driven shows.
  The relay default state is Matter's StartUpOnOff (owner's decision,
  2026-09-12); the legacy `/api/relays/safe_state` endpoint was removed;
- every console line that marks a known defect or a failure: FRAM RDID,
  LittleFS formatting, settings that failed to load, MCUboot errors, faults,
  and a second bootloader banner inside the settle window.

Usage:
    boot_cycle.py OUT_DIR --warm 10
    boot_cycle.py OUT_DIR --cold 10
"""

import argparse
import json
import re
import subprocess
import time
from pathlib import Path

import bench_console as bc

CLI = ("/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/"
       "STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI")

MILESTONES = [
    ("jump", r"Jumping to the first image slot"),
    ("zephyr", r"Booting Zephyr OS"),
    ("main", r"Start main app"),
    ("psram", r"PSRAM @0x[0-9a-f]+: (write/read \d+ words OK|\d+ of \d+ words mismatch)|PSRAM: allocation"),
    ("net_up", r"Network interface up with MAC"),
    ("hosted", r"coprocessor firmware v\d"),
    # Matter logs at WRN on this build; the DNS-SD error below is printed once
    # by server init on every boot, so it serves as the "Matter started" mark.
    ("matter_init", r"DNS-SD advertising not available\. DNS-SD init disabled"),
]

FINDINGS = {
    "fram_rdid_fail": r"mb85rsxx: (invalid device ID|Failed to initialize device|failed to read RDID)",
    "lfs_format": r"can't mount \(LFS|format failed|remount after format failed",
    "settings_load_fail": r"Failed to load topik|settings registry init failed",
    "mcuboot_error": r"Unable to find bootable image|is not valid|secondary slot -> primary|copying the secondary",
    "fatal": r"ZEPHYR FATAL ERROR|\*\*\*\*\* .*FAULT",
    "hosted_rpc_timeout": r"RPC msg_id \d+ timed out",
}

# (label, GPIO port ODR address, bit)
RELAY_PINS = [
    ("relay1_PA5", 0x42020014, 5),
    ("relay2_PC4", 0x42020814, 4),
    ("relay3_PC5", 0x42020814, 5),
    ("relay4_PB2", 0x42020414, 2),
]


def http_get_json(host, path, timeout=3):
    r = subprocess.run(["curl", "-s", "-m", str(timeout), f"http://{host}{path}"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        return {"_unparsed": r.stdout[:200]}


def wait_http(host, timeout):
    """Wait the way a browser user would, not the way a load generator would.

    Polling with short timeouts poisons the measurement on this server: every
    abandoned connection holds one of four client slots for the 10 s inactivity
    timeout, so a fresh request every two seconds keeps the server busy with
    corpses (measured 2026-09-12: 80 of 80 requests timed out, the next one
    after polling stopped succeeded). So: ping until IPv4 answers, then one
    request at a time with a long timeout.

    Returns (IPv4 reachable host time, HTTP ready host time, {outcome: count}).
    """
    start = time.time()
    failures = {}
    reachable = None
    while time.time() - start < timeout:
        r = subprocess.run(["ping", "-c", "1", "-W", "500", host],
                           capture_output=True, text=True)
        if r.returncode == 0:
            reachable = time.time()
            break
        time.sleep(0.5)
    if reachable is None:
        return None, None, {"ping": "no reply"}
    while time.time() - start < timeout:
        r = subprocess.run(["curl", "-s", "-m", "20", "-o", "/dev/null", "-w", "%{http_code}",
                            f"http://{host}/api/relays/state"],
                           capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip() == "200":
            return reachable, time.time(), failures
        key = f"rc{r.returncode}/http{r.stdout.strip() or '-'}"
        failures[key] = failures.get(key, 0) + 1
        time.sleep(1.0)
    return reachable, None, failures


def hard_reset(log_path):
    r = subprocess.run([CLI, "-c", "port=swd", "sn=" + bc.ST_LINK_SERIAL,
                        "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
    with open(log_path, "a") as f:
        f.write(r.stdout + r.stderr)
    return r.returncode


def read_register(console, address):
    text = console.collect(f"devmem 0x{address:08X}", quiet=0.4, timeout=5) or ""
    m = re.search(r"Read value (0x[0-9a-fA-F]+)", text)
    return int(m.group(1), 16) if m else None


def one_boot(console, index, mode, args, out):
    rec = {"index": index, "mode": mode}
    since = console.mark()

    if mode == "warm":
        rec["trigger_host_time"] = time.time()
        rc = hard_reset(out / "cubeprogrammer.log")
        if rc != 0:
            time.sleep(2)
            rc = hard_reset(out / "cubeprogrammer.log")
        rec["reset_rc"] = rc
        banner = console.wait_for(r"Starting bootloader", since, 20)
    else:
        print(f"[boot {index}] ПОРА: снимите питание платы на 5 с и включите", flush=True)
        (out / "status.txt").write_text(f"boot {index}: waiting for power cycle\n")
        banner = console.wait_for(r"Starting bootloader", since, args.cold_wait)

    if banner is None:
        rec["verdict"] = "FAIL"
        rec["why"] = ["no MCUboot banner"]
        return rec
    t0 = banner[0]
    if mode == "warm":
        rec["trigger_to_bootloader_s"] = round(t0 - rec["trigger_host_time"], 2)

    for name, pattern in MILESTONES:
        if name == "matter_init":
            continue  # late; looked up after the settle window
        hit = console.wait_for(pattern, banner[2], args.boot_timeout)
        rec[f"{name}_s"] = round(hit[0] - t0, 2) if hit else None
        if name == "psram":
            rec["psram_ok"] = bool(hit and "OK" in hit[1].group(0))

    reachable, ready, http_failures = wait_http(args.host, args.http_timeout)
    rec["ipv4_reachable_s"] = round(reachable - t0, 2) if reachable else None
    rec["http_ready_s"] = round(ready - t0, 2) if ready else None
    rec["http_failures_before_ready"] = http_failures

    # Let late initialisation (Matter, DHCP renewals) happen before judging.
    time.sleep(max(0.0, args.settle - (time.time() - t0)))

    uptime_text = console.collect("kernel uptime", quiet=0.5, timeout=5) or ""
    m = re.search(r"Uptime: (\d+) ms", uptime_text)
    rec["uptime_ms"] = int(m.group(1)) if m else None

    iface = console.collect("net iface", quiet=1.0, timeout=10) or ""
    eth = iface.split("Interface wlan0")[0]
    rec["carrier_on"] = "carrier=ON" in eth
    rec["dhcp_bound"] = bool(re.search(r"DHCPv4 state\s*: bound", eth))
    ip = re.search(r"(\d+\.\d+\.\d+\.\d+)/\S+ DHCP", eth)
    rec["ipv4"] = ip.group(1) if ip else None

    scan = console.collect("wifi scan", quiet=4.0, timeout=40) or ""
    rec["wifi_scan_ok"] = "Scan request done" in scan and "Scan request failed" not in scan

    rec["relays_state"] = http_get_json(args.host, "/api/relays/state")

    odr = {}
    for label, address, bit in RELAY_PINS:
        value = odr.setdefault(address, read_register(console, address))
        rec[label + "_odr"] = None if value is None else (value >> bit) & 1

    matter = console.wait_for(MILESTONES[-1][1], banner[2], 0)
    rec["matter_init_s"] = round(matter[0] - t0, 2) if matter else None

    lines = console.lines_since(banner[2])
    for name, pattern in FINDINGS.items():
        rx = re.compile(pattern)
        rec[name] = sum(1 for _, text in lines if rx.search(text))
    rec["bootloader_banners"] = sum(1 for _, t in lines if "Starting bootloader" in t)
    rec["err_lines"] = [t[:160] for _, t in lines if "<err>" in t]

    why = []
    for key in ("main_s", "http_ready_s", "net_up_s", "hosted_s"):
        if rec[key] is None:
            why.append(f"{key} missing")
    for key in ("psram_ok", "carrier_on", "dhcp_bound", "wifi_scan_ok", "relays_state"):
        if not rec[key]:
            why.append(f"{key} false")
    for key in ("lfs_format", "settings_load_fail", "mcuboot_error", "fatal"):
        if rec[key]:
            why.append(key)
    if rec["bootloader_banners"] != 1:
        why.append(f"{rec['bootloader_banners']} bootloader banners")
    rec["verdict"] = "PASS" if not why else "FAIL"
    rec["why"] = why
    return rec


def write_table(records, path):
    cols = ["index", "mode", "verdict", "trigger_to_bootloader_s", "jump_s", "main_s",
            "psram_ok", "net_up_s", "hosted_s", "ipv4_reachable_s", "http_ready_s", "matter_init_s", "dhcp_bound",
            "wifi_scan_ok", "fram_rdid_fail", "lfs_format",
            "settings_load_fail", "fatal", "relay1_PA5_odr", "relay2_PC4_odr",
            "relay3_PC5_odr", "relay4_PB2_odr", "why"]
    lines = ["| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in records:
        cells = []
        for c in cols:
            v = r.get(c)
            cells.append("; ".join(v) if isinstance(v, list) else ("" if v is None else str(v)))
        lines.append("| " + " | ".join(cells) + " |")
    path.write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--warm", type=int, default=0)
    ap.add_argument("--cold", type=int, default=0)
    ap.add_argument("--first-index", type=int, default=1)
    ap.add_argument("--host", default="192.168.88.14")
    ap.add_argument("--boot-timeout", type=float, default=90)
    ap.add_argument("--http-timeout", type=float, default=180)
    ap.add_argument("--settle", type=float, default=40,
                    help="seconds after the MCUboot banner before the checks")
    ap.add_argument("--cold-wait", type=float, default=900)
    args = ap.parse_args()

    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    console = bc.Console(out / "console.log")
    time.sleep(1.0)

    plan = ["warm"] * args.warm + ["cold"] * args.cold
    records = []
    with open(out / "boots.jsonl", "a") as jl:
        for n, mode in enumerate(plan):
            index = args.first_index + n
            rec = one_boot(console, index, mode, args, out)
            records.append(rec)
            jl.write(json.dumps(rec, ensure_ascii=False) + "\n")
            jl.flush()
            write_table(records, out / "boots.md")
            print(f"[boot {index}] {mode} {rec['verdict']} main={rec.get('main_s')} "
                  f"http={rec.get('http_ready_s')} why={rec.get('why')}", flush=True)
    console.close()
    passed = sum(1 for r in records if r["verdict"] == "PASS")
    print(f"{passed}/{len(records)} passed")


if __name__ == "__main__":
    main()
