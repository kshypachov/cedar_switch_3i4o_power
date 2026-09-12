#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Unattended network and PSRAM smoke run against the bench board.

Runs for a fixed time without touching the board's state: no resets, no
writes, only a shell probe, ping and HTTP GET of the index page. What it is
built to catch, in order of how the failures actually looked on this board:

- A reset. Uptime must only grow; a boot banner, a fault dump or a VCP drop
  in the console log is recorded as an event.
- Starvation. The W5500 RX thread runs cooperatively and, under enough
  traffic, starves every thread below it: the shell and the log go silent
  while ping and HTTP keep working. A shell probe with a timeout is the
  direct detector, so its latency is recorded for every probe, not averaged.
- Latency drift. Ping and HTTP are measured exactly the way the 2026-09-12
  baseline was (docs/upstream/w5500-filtering-research.md gives the numbers;
  the method is recorded in README.md here): `ping -c 20 -i 0.3 -W 2000` and
  15 sequential `curl -w %{time_total}` of `/`, with the same percentile
  index arithmetic. Change any of that and the comparison stops meaning
  anything.

Application data, heaps and most thread stacks live in PSRAM on this build,
so the run doubles as a soak of that mapping.

Usage:
    smoke.py OUT_DIR [--duration 7200] [--probe-interval 10] [--net-interval 300]
"""

import argparse
import csv
import json
import re
import subprocess
import threading
import time
from pathlib import Path

import bench_console as bc

BASELINE = {"http_p50": 0.053, "http_p95": 0.056, "ping_min": 8.5, "ping_max": 11.6}
TARGET_HTTP_P95 = 0.250

EVENT_PATTERNS = {
    "bootloader": r"Starting bootloader",
    "zephyr_boot": r"Booting Zephyr OS",
    "app_start": r"Start main app",
    "fatal": r"ZEPHYR FATAL ERROR|\*\*\*\*\* .*FAULT",
    "lfs_format": r"can't mount \(LFS|format failed|remount after format failed",
    "console_lost": r"^\[bench: console lost",
}

STACK_LINE = re.compile(
    r"^\s*(0x[0-9a-f]+)\s+(.*?)\s*\(real size (\d+)\):\s+unused\s+(\d+)\s+"
    r"usage\s+(\d+) / (\d+) \(\s*(\d+) %\)"
)


def baseline_percentiles(values):
    """p50/p95 with the baseline one-liner's index arithmetic.

    The baseline used awk on sorted times: p50 = a[int(NR*0.5)+1] and
    p95 = a[int(NR*0.95)], 1-based. For 15 samples that is the 8th and 14th.
    """
    a = sorted(values)
    n = len(a)
    if n == 0:
        return None
    return {
        "n": n,
        "min": a[0],
        "p50": a[int(n * 0.5)],
        "p95": a[max(int(n * 0.95) - 1, 0)],
        "max": a[-1],
    }


def nearest_rank(values, q):
    a = sorted(values)
    if not a:
        return None
    k = max(0, min(len(a) - 1, int(round(q * len(a) + 0.5)) - 1))
    return a[k]


def http_round(host, count=15):
    times, failures = [], 0
    for _ in range(count):
        r = subprocess.run(
            ["curl", "-s", "-m", "8", "-o", "/dev/null", "-w", "%{time_total}\n",
             f"http://{host}/"],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            failures += 1
            continue
        times.append(float(r.stdout.strip()))
    return times, failures


def ping_round(host):
    r = subprocess.run(["ping", "-c", "20", "-i", "0.3", "-W", "2000", host],
                       capture_output=True, text=True)
    out = r.stdout
    tx = rx = 0
    m = re.search(r"(\d+) packets transmitted, (\d+) packets received", out)
    if m:
        tx, rx = int(m.group(1)), int(m.group(2))
    rtt = re.search(r"= ([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+) ms", out)
    mn = avg = mx = None
    if rtt:
        mn, avg, mx = (float(rtt.group(i)) for i in (1, 2, 3))
    return tx, rx, mn, avg, mx


def route_interface(host):
    r = subprocess.run(["route", "-n", "get", host], capture_output=True, text=True)
    m = re.search(r"interface: (\S+)", r.stdout)
    return m.group(1) if m else None


def parse_stacks(text):
    stacks = {}
    for line in (text or "").splitlines():
        m = STACK_LINE.match(line)
        if m:
            addr, name, size, _unused, used = m.group(1, 2, 3, 4, 5)
            stacks[f"{name or '(unnamed)'}@{addr}"] = (int(used), int(size))
    return stacks


def snapshot(console, path):
    parts = []
    for cmd in ("kernel uptime", "kernel thread stacks", "net iface", "wifi status",
                "net stats", "kernel heap"):
        parts.append(f"### {cmd}\n{console.collect(cmd, quiet=1.0, timeout=15) or '(no answer)'}\n")
    text = "\n".join(parts)
    path.write_text(text)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--host", default="192.168.88.14")
    ap.add_argument("--iface", default="en7", help="host interface the route must use")
    ap.add_argument("--duration", type=float, default=7200)
    ap.add_argument("--probe-interval", type=float, default=10)
    ap.add_argument("--probe-timeout", type=float, default=5)
    ap.add_argument("--net-interval", type=float, default=300)
    args = ap.parse_args()

    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    started = time.time()
    console = bc.Console(out / "console.log")
    time.sleep(1.0)

    iface = route_interface(args.host)
    start_text = snapshot(console, out / "snapshot-start.txt")
    events_from = console.mark()

    shell_rows, ping_rows, http_rows = [], [], []
    stop = threading.Event()

    def network_loop():
        rnd = 0
        while not stop.is_set():
            rnd += 1
            t = time.time()
            ping_rows.append((rnd, t, *ping_round(args.host)))
            times, failures = http_round(args.host)
            http_rows.append((rnd, t, times, failures))
            stop.wait(max(0.0, args.net_interval - (time.time() - t)))

    net_thread = threading.Thread(target=network_loop, daemon=True)
    net_thread.start()

    deadline = started + args.duration
    while time.time() < deadline:
        t = time.time()
        latency, m = console.probe("kernel uptime", r"Uptime: (\d+) ms", args.probe_timeout)
        shell_rows.append((t, latency, int(m.group(1)) if m else None))
        time.sleep(max(0.0, args.probe_interval - (time.time() - t)))

    stop.set()
    net_thread.join(timeout=60)
    event_lines = console.lines_since(events_from)
    end_text = snapshot(console, out / "snapshot-end.txt")
    console.close()

    # -- raw samples -----------------------------------------------------
    with open(out / "shell.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["host_time", "elapsed_s", "latency_s", "uptime_ms"])
        for t, lat, up in shell_rows:
            w.writerow([f"{t:.3f}", f"{t - started:.1f}",
                        "" if lat is None else f"{lat:.4f}", "" if up is None else up])
    with open(out / "ping.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["round", "host_time", "sent", "received", "min_ms", "avg_ms", "max_ms"])
        w.writerows(ping_rows)
    with open(out / "http.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["round", "host_time", "failures", "times_s"])
        for rnd, t, times, failures in http_rows:
            w.writerow([rnd, f"{t:.3f}", failures, " ".join(f"{x:.4f}" for x in times)])

    # -- analysis --------------------------------------------------------
    events = []
    for name, pattern in EVENT_PATTERNS.items():
        rx = re.compile(pattern)
        for t, text in event_lines:
            if rx.search(text):
                events.append({"t": round(t - started, 1), "event": name, "line": text[:200]})
    events.sort(key=lambda e: e["t"])

    answered = [lat for _, lat, _ in shell_rows if lat is not None]
    uptimes = [(t, up) for t, _, up in shell_rows if up is not None]
    regressions = sum(1 for (_, a), (_, b) in zip(uptimes, uptimes[1:]) if b < a)

    longest_stall, current = 0.0, None
    for t, lat, _ in shell_rows:
        if lat is None:
            current = current if current is not None else t
        elif current is not None:
            longest_stall = max(longest_stall, t - current)
            current = None
    if current is not None:
        longest_stall = max(longest_stall, shell_rows[-1][0] - current)

    all_http = [x for _, _, times, _ in http_rows for x in times]
    sent = sum(r[2] for r in ping_rows)
    received = sum(r[3] for r in ping_rows)
    ping_mins = [r[4] for r in ping_rows if r[4] is not None]
    ping_avgs = [r[5] for r in ping_rows if r[5] is not None]
    ping_maxs = [r[6] for r in ping_rows if r[6] is not None]

    s_start, s_end = parse_stacks(start_text), parse_stacks(end_text)

    summary = {
        "host_interface": iface,
        "expected_interface": args.iface,
        "duration_s": round(time.time() - started),
        "console_reopens": sum(1 for e in events if e["event"] == "console_lost"),
        "uptime_regressions": regressions,
        "uptime_first_ms": uptimes[0][1] if uptimes else None,
        "uptime_last_ms": uptimes[-1][1] if uptimes else None,
        "events": events,
        "shell": {
            "probes": len(shell_rows),
            "answered": len(answered),
            "timeouts": len(shell_rows) - len(answered),
            "p50_s": nearest_rank(answered, 0.50),
            "p95_s": nearest_rank(answered, 0.95),
            "p99_s": nearest_rank(answered, 0.99),
            "max_s": max(answered) if answered else None,
            "over_100ms": sum(1 for x in answered if x > 0.1),
            "over_1s": sum(1 for x in answered if x > 1.0),
            "longest_stall_s": round(longest_stall, 1),
        },
        "ping": {
            "rounds": len(ping_rows),
            "sent": sent,
            "received": received,
            "loss_pct": round(100.0 * (sent - received) / sent, 2) if sent else None,
            "min_ms": min(ping_mins) if ping_mins else None,
            "avg_of_avgs_ms": round(sum(ping_avgs) / len(ping_avgs), 3) if ping_avgs else None,
            "max_ms": max(ping_maxs) if ping_maxs else None,
        },
        "http": {
            "rounds": len(http_rows),
            "failures": sum(r[3] for r in http_rows),
            "overall": baseline_percentiles(all_http),
            "worst_round_p95_s": max((baseline_percentiles(t)["p95"] for _, _, t, _ in http_rows if t),
                                     default=None),
        },
        "stacks": {k: {"start": s_start.get(k), "end": s_end.get(k)}
                   for k in sorted(set(s_start) | set(s_end))},
        "baseline": BASELINE,
        "target_http_p95_s": TARGET_HTTP_P95,
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False))
    print(json.dumps({k: v for k, v in summary.items() if k != "stacks"}, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
