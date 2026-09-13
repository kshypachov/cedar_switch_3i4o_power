"""Split every commissioning in <backend>.console.raw into phases by the device's log.

usage: phases.py [backend ...]   (default: file zms nvs fcb)

Phases (seconds of device uptime between log lines, same boot):
  pase         "Commissioning session establishment step started" -> "... completed ..."   SPAKE2+
  attestation  "Received ArmFailSafe" -> "AttestationRequest successful"                     DAC signature
  csr          "AttestationRequest successful" -> "CSRRequest successful"                   P-256 keygen + CSR
  noc_to_done  "CSRRequest successful" -> "Received CommissioningComplete"                  NOC chain check, storage, CASE
  commit       "Received CommissioningComplete" -> "was committed to storage"               fabric write
Crypto-bound phases should not depend on the settings backend; storage-bound ones should.
"""
import re, statistics, sys
from pathlib import Path

HERE = Path(__file__).parent
MARKS = [("window", "Commissioning window is open"), ("pase_start", "Commissioning session establishment step started"),
         ("pase_done", "Commissioning completed session establishment step"), ("failsafe", "Received ArmFailSafe"),
         ("attest", "AttestationRequest successful"), ("csr", "CSRRequest successful"),
         ("complete", "Received CommissioningComplete"), ("commit", "was committed to storage")]
UP = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)")
PHASES = [("pase", "pase_start", "pase_done"), ("attestation", "failsafe", "attest"), ("csr", "attest", "csr"),
          ("noc_to_done", "csr", "complete"), ("commit", "complete", "commit")]

def runs(backend):
    raw = (HERE / f"{backend}.console.raw").read_bytes().decode("latin-1")
    raw = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", raw).replace("\r", "")
    current, out = None, []
    for line in raw.splitlines():
        m = UP.search(line)
        if not m:
            continue
        t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000
        for key, needle in MARKS:
            if needle in line:
                if key == "window":
                    if current:
                        out.append(current)
                    current = {"window": t}
                elif current is not None and key not in current:
                    current[key] = t
    if current:
        out.append(current)
    return out

for b in sys.argv[1:] or ["file", "zms", "nvs", "fcb"]:
    if not (HERE / f"{b}.console.raw").exists():
        continue
    rs = runs(b)
    print(f"\n## {b}: commissioning phases, s (device uptime)\n")
    print("| # | " + " | ".join(p for p, _, _ in PHASES) + " |")
    print("|---|" + "---|" * len(PHASES))
    cols = {p: [] for p, _, _ in PHASES}
    for i, r in enumerate(rs, 1):
        cells = []
        for p, a, z in PHASES:
            v = r[z] - r[a] if a in r and z in r and r[z] >= r[a] else None
            if v is not None:
                cols[p].append(v)
            cells.append("—" if v is None else f"{v:.1f}")
        print(f"| {i} | " + " | ".join(cells) + " |")
    print("| median | " + " | ".join(f"{statistics.median(v):.1f}" if v else "—" for v in cols.values()) + " |")
