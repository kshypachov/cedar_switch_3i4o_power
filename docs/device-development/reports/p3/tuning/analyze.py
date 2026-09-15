"""Metrics of one tuning trial: tuning/<backend>/<trial>/ -> metrics.json.

usage: analyze.py <trial dir> [...]

Score (frozen before the sweep, see README.md), lower is better:
  score = median(Matter init with fabric) + median(fabric removal) + median(commit phase)
over the trial's commissioning cycles; slope = (with-fabric init of the last
cycle - that of the first) / (cycles - 1), seconds per cycle.

Commissioning phases, seconds of device uptime between log lines of one boot
(as settings-backends/phases.py):
  pase         "Commissioning session establishment step started" -> "... completed ..."   SPAKE2+
  attestation  "Received ArmFailSafe" -> "AttestationRequest successful"                     DAC signature
  csr          "AttestationRequest successful" -> "CSRRequest successful"                   P-256 keygen + CSR
  noc_to_done  "CSRRequest successful" -> "Received CommissioningComplete"                  NOC chain, storage, CASE
  commit       "Received CommissioningComplete" -> "was committed to storage"               fabric write
"""
import gzip, json, re, statistics, sys
from pathlib import Path

UP = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)")
MARKS = [("window", "Commissioning window is open"), ("pase_start", "Commissioning session establishment step started"),
         ("pase_done", "Commissioning completed session establishment step"), ("failsafe", "Received ArmFailSafe"),
         ("attest", "AttestationRequest successful"), ("csr", "CSRRequest successful"),
         ("complete", "Received CommissioningComplete"), ("commit", "was committed to storage")]
PHASES = [("pase", "pase_start", "pase_done"), ("attestation", "failsafe", "attest"), ("csr", "attest", "csr"),
          ("noc_to_done", "csr", "complete"), ("commit", "complete", "commit")]
BENCH = re.compile(r"(open\+close|read 32 \+seek|read 32|read 1024|scan \(no match\)|scan \(all\)|load_one \(hit\)|load_one \(miss\)|val_len \(hit\)|val_len \(miss\))\s+(\d+)\s*/\s*(\d+)(?:\s*->\s*(-?\d+))?")


def median(xs):
    xs = [x for x in xs if x is not None]
    return round(statistics.median(xs), 3) if xs else None


def console_text(d):
    gz, raw = d / "console.log.gz", d / "console.raw"
    data = gzip.open(gz).read() if gz.exists() else raw.read_bytes()
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", data.decode("latin-1")).replace("\r", "")


def phases(text):
    current, runs = None, []
    for line in text.splitlines():
        m = UP.search(line)
        if not m:
            continue
        t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000
        for key, needle in MARKS:
            if needle in line:
                if key == "window":
                    if current:
                        runs.append(current)
                    current = {"window": t}
                elif current is not None and key not in current:
                    current[key] = t
    if current:
        runs.append(current)
    out = []
    for r in runs:
        out.append({p: (round(r[z] - r[a], 3) if a in r and z in r and r[z] >= r[a] else None) for p, a, z in PHASES})
    return out


def analyze(d):
    ev = [json.loads(l) for l in (d / "trial.jsonl").read_text().splitlines() if l.strip()]
    meta = json.loads((d / "meta.json").read_text())
    boots = [e for e in ev if e["event"] == "boot"]
    comm = [e for e in ev if e["event"] == "commission"]
    rem = [e for e in ev if e["event"] == "remove_fabrics"]
    cycles = meta.get("cycles", 5)
    per = []
    for i in range(1, cycles + 1):
        bb = next((e for e in boots if e["label"] == f"cycle {i}: before commissioning"), {})
        bw = next((e for e in boots if e["label"] == f"cycle {i}: with fabric"), {})
        c = comm[i - 1] if i - 1 < len(comm) else {}
        r = rem[i - 1] if i - 1 < len(rem) else {}
        per.append(dict(cycle=i, init_before_s=bb.get("matter_init_s"), window_open_s=c.get("window_open_s"),
                        commission_ok=c.get("ok"), commission_s=c.get("commission_s"),
                        init_with_fabric_s=bw.get("matter_init_s"), remove_s=r.get("remove_s")))
    text = console_text(d)
    ph = phases(text)
    # Every boot of the trial shows the bootloader; one is the start after flashing.
    # More starts than measured boots mean resets the method did not intend.
    unexpected_boots = text.count("Starting bootloader") - 1 - len(boots)
    bench = {}
    for e in ev:
        if e["event"] == "bench":
            for l in e.get("text", []):
                m = BENCH.search(l)
                if m:
                    bench[m[1]] = dict(best_us=int(m[2]), mean_us=int(m[3]), result=None if m[4] is None else int(m[4]))
    withf = [p["init_with_fabric_s"] for p in per]
    before = [p["init_before_s"] for p in per]
    commit = [p["commit"] for p in ph]
    parts = dict(init_with_fabric=median(withf), remove=median([p["remove_s"] for p in per]), commit=median(commit))
    complete = all(v is not None for v in parts.values())

    def slope(xs):
        return round((xs[-1] - xs[0]) / (len(xs) - 1), 3) if len(xs) > 1 and xs[0] is not None and xs[-1] is not None else None

    back = next((e for e in ev if e["event"] == "structure_read_back"), {})
    pers = next((e for e in ev if e["event"] == "persistence"), {})
    wipe = next((e for e in ev if e["event"] == "wipe"), {})
    clean = next((e for e in boots if e["label"] == "clean partition"), {})
    aborted = next((e["reason"] for e in ev if e["event"] == "aborted"), None)
    m = dict(meta=meta, aborted=aborted,
             score=round(sum(parts.values()), 3) if complete else None, score_parts=parts,
             slope_with_fabric_s_per_cycle=slope(withf), slope_before_s_per_cycle=slope(before),
             median_init_before_s=median(before), median_window_open_s=median([p["window_open_s"] for p in per]),
             commissioning_ok=sum(1 for p in per if p["commission_ok"]), commissioning_total=len(comm),
             phase_medians={p: median([r[p] for r in ph]) for p, _, _ in PHASES},
             clean_boot_init_s=clean.get("matter_init_s"), wipe=wipe.get("result"),
             read_back_ok=bool(back.get("unique_id_same") and back.get("counter_advanced")),
             persistence_ok=pers.get("ok"), persistence=f"{pers.get('reboot_count')} counted / {pers.get('boots_since_wipe')} boots" if pers else None,
             boot_failures=sum(1 for e in ev if e["event"] == "boot_failed"),
             unexpected_boots=unexpected_boots,
             storage_warnings=sorted({w[-160:] for e in boots for w in e.get("storage_warnings", [])})[:8],
             bench=bench, cycles=per, phases=ph)
    (d / "metrics.json").write_text(json.dumps(m, indent=1, ensure_ascii=False) + "\n")
    return m


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        m = analyze(Path(arg))
        print(f"{arg}: score {m['score']} {m['score_parts']} slope {m['slope_with_fabric_s_per_cycle']} "
              f"comm {m['commissioning_ok']}/{m['commissioning_total']} phases {m['phase_medians']} "
              f"persistence {m['persistence']} aborted {m['aborted']}")
