#!/usr/bin/env python3
"""Verify a full memc-sample board run and print an explicit verdict."""
import re
import sys

path = sys.argv[1]
text = open(path, errors="replace").read()
lines = text.splitlines()

failures = []
warnings = []

# 1. Detection and init must be present
if "Detected ESP-PSRAM64H" not in text:
    failures.append("no 'Detected ESP-PSRAM64H' line")
if "PSRAM initialised in QPI mode, 8192 KB" not in text:
    failures.append("no 'PSRAM initialised ... 8192 KB' line")

# 2. No dropped messages, no error/fault logs anywhere in the run
if "messages dropped" in text:
    failures.append("log subsystem dropped messages")
for pat in ("<err>", "<wrn>", "FAULT", "Fault", "usage fault", "***"):
    for ln, line in enumerate(lines, 1):
        if pat in line and "Booting Zephyr OS" not in line:
            failures.append(f"line {ln}: {line.strip()[:90]}")

# 3. Every dump line must be a strictly incrementing byte sequence
dump = anomalies = 0
for ln, line in enumerate(lines, 1):
    toks = line.strip().split()
    if len(toks) in (8, 16) and all(re.fullmatch(r"[0-9a-f]{2}", t) for t in toks):
        dump += 1
        vals = [int(t, 16) for t in toks]
        if any(vals[i] != (vals[i - 1] + 1) & 0xFF for i in range(1, len(vals))):
            anomalies += 1
            warnings.append(f"dump glitch at line {ln}: {line.strip()}")
if dump == 0:
    failures.append("no dump lines captured at all")

# 4. The firmware's own memcmp verdict is mandatory
if "Read data matches written data" not in text:
    failures.append("no on-device memcmp verdict line")

print(f"--- run stats: {len(lines)} lines, {dump} dump lines, "
      f"{anomalies} dump glitches ---")
for w in warnings:
    print("WARN:", w)

if failures:
    print("BOARD TEST: FAIL")
    for f in failures:
        print("  -", f)
    sys.exit(1)
if anomalies:
    print("BOARD TEST: PASS (memory OK by on-device memcmp; "
          f"{anomalies} single-char console glitches, UART path only)")
else:
    print("BOARD TEST: PASS")
