#!/usr/bin/env python3
"""Analyse one full stress-test capture against per-mode expectations.

usage: analyze_stress.py <log> <auto|generic|named>
"""
import re
import sys

path, mode = sys.argv[1], sys.argv[2]
raw = open(path, errors="replace").read().splitlines()

# A boot starts with the MSPI controller's config line; analyse the last boot
# only, so the tail of a previous run still on the board is ignored.
starts = [i for i, l in enumerate(raw) if "MSPI config result" in l]
if not starts:
    print("VERDICT: FAIL - no boot found in capture")
    sys.exit(1)
lines = raw[starts[-1]:]
skipped = starts[-1]

fails, notes = [], []

expect = {
    "auto":    ["Detected IS66WVS4M8BLL (ID 9D 5D 40 C1 40)"],
    "generic": ["Generic PSRAM: skipping KGD check (ID MF=0x9D KGD=0x5D EID=0x40)"],
    "named":   [],
}[mode]
forbid = {
    "auto":    ["Generic PSRAM"],
    "generic": ["Detected "],
    "named":   ["Detected ", "Generic PSRAM"],
}[mode]

text = "\n".join(lines)
for e in expect:
    if e not in text:
        fails.append(f"expected init line missing: {e}")
for f in forbid:
    if f in text:
        fails.append(f"line must not appear in {mode} mode: {f}")
if "PSRAM initialised in QPI mode, 4096 KB" not in text:
    fails.append("no 'initialised in QPI mode, 4096 KB' line")
if "Device psram@0: 4096 KB mapped at 0x90000000" not in text:
    fails.append("test app did not see 4096 KB at 0x90000000")
if "differs from the detected" in text:
    notes.append("DT size mismatch warning present")

cyc = [(int(m.group(1)), m.group(2), int(m.group(3)))
       for l in lines
       if (m := re.match(r"Cycle\s+(\d+)/100: (\w+) \((\d+) ms\)", l))]
nums = [c[0] for c in cyc]
if nums != list(range(1, 101)):
    fails.append(f"cycle sequence broken: {len(nums)} lines, first={nums[:1]} last={nums[-1:]}")
bad = [c for c in cyc if c[1] != "ok"]
if bad:
    fails.append(f"non-ok cycles: {bad[:5]}")
times = [c[2] for c in cyc]

for pat in ("MISMATCH", "dropped", "E: ", "<err>", "TEST FAIL", "fault"):
    hits = [l for l in lines if pat in l]
    if hits:
        fails.append(f"'{pat}' present: {hits[0][:80]}")

known = re.compile(r"^(I: |W: |Cycle\s+\d+/100: ok |Total time: |TEST PASS|"
                   r"\*\*\* Booting|PSRAM full-volume|Device psram@0:|$)")
odd = [l for l in lines if not known.match(l)]
if odd:
    fails.append(f"unexpected lines: {odd[:3]}")

if "TEST PASS: 100 cycles, 4096 KB verified twice per cycle, 0 errors" not in text:
    fails.append("device verdict line missing or wrong")

print(f"mode={mode}: {len(lines)} lines analysed ({skipped} pre-boot lines skipped), "
      f"cycles={len(cyc)}, "
      f"cycle ms min/max={min(times) if times else '-'}/{max(times) if times else '-'}")
for n in notes:
    print("note:", n)
if fails:
    print(f"VERDICT [{mode}]: FAIL")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print(f"VERDICT [{mode}]: PASS")
