"""Regenerate tuning/<backend>/results.md from every trial's metrics.json.

usage: results.py <backend>

Decision rule (frozen before the sweep, see README.md):
  reference  = median score of the backend's base runs (kind "base")
  noise      = max - min of the scores of the first three base runs
  better     score < reference - noise
  worse      score > reference + noise
  noise      otherwise
Mean / max columns are a reading aid for sawtooth backends (File): the
median of 5 cycles depends on where the compaction peaks fall; the score is
unchanged. A trial that aborted, lost a commissioning or failed the persistence check is
marked as such and not compared. Owner's notes per trial go in notes.md next
to its metrics (first line is shown in the table).
"""
import json, statistics, sys
from pathlib import Path

TUNING = Path(__file__).resolve().parent
backend = sys.argv[1]
root = TUNING / backend
trials = []
for d in sorted(p for p in root.iterdir() if p.is_dir()):
    mf = d / "metrics.json"
    if mf.exists():
        trials.append((d, json.loads(mf.read_text())))


def f(v, nd=2):
    return "—" if v is None else f"{v:.{nd}f}"


def valid(m):
    return (m["score"] is not None and not m["aborted"] and m["commissioning_ok"] == m["meta"]["cycles"]
            and m["persistence_ok"] and m["read_back_ok"] and m.get("unexpected_boots", 0) == 0)


bases = [m for _, m in trials if m["meta"]["kind"] == "base" and valid(m)]
first3 = [m["score"] for m in bases[:3]]
reference = statistics.median([m["score"] for m in bases]) if bases else None
noise = (max(first3) - min(first3)) if len(first3) == 3 else None

lines = [f"# {backend}: tuning results", "",
         "Score = median Matter init with fabric + median fabric removal + median `commit` phase over the cycles, s (lower is better).",
         "Slope = (with-fabric init, last cycle - first cycle) / (cycles - 1), s per cycle.",
         f"Reference = median of {len(bases)} base runs: **{f(reference)} s**; noise = range of the first three base scores: "
         f"**{f(noise)} s**" + ("" if noise is not None else " (fewer than three base runs yet)") + ".", "",
         "| # | kind | change | score | init w/ fabric | removal | commit | slope | init w/ fabric mean / max | removal mean / max | init before | window | pase / att / csr | noc_to_done | load_one hit / miss, ms | val_len hit / miss, ms | scan all, ms | comm | persist | decision | note |",
         "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
for d, m in trials:
    meta, p, ph, b = m["meta"], m["score_parts"], m["phase_medians"], m["bench"]

    def mm(key):
        xs = [c[key] for c in m["cycles"] if c.get(key) is not None]
        return f"{sum(xs) / len(xs):.1f} / {max(xs):.1f}" if xs else "—"

    def ms(key):
        return f"{b[key]['mean_us'] / 1000:.1f}" if key in b else "—"

    if m["aborted"]:
        decision = f"failed: {m['aborted']}"
    elif not valid(m):
        decision = "failed: " + ", ".join(x for x, bad in [("commissioning", m["commissioning_ok"] != meta["cycles"]),
                                                          ("persistence", not m["persistence_ok"]),
                                                          ("read back", not m["read_back_ok"]),
                                                          (f"{m.get('unexpected_boots')} unexpected boots", m.get("unexpected_boots", 0) != 0),
                                                          ("incomplete", m["score"] is None)] if bad)
    elif meta["kind"] == "base":
        decision = "base"
    elif reference is None or noise is None:
        decision = "pending base"
    elif m["score"] < reference - noise:
        decision = f"**better** ({m['score'] - reference:+.2f})"
    elif m["score"] > reference + noise:
        decision = f"worse ({m['score'] - reference:+.2f})"
    else:
        decision = f"noise ({m['score'] - reference:+.2f})"
    note_file = d / "notes.md"
    note = note_file.read_text().splitlines()[0] if note_file.exists() else ""
    lines.append(f"| {d.name} | {meta['kind']} | {meta['change'] or '—'} | {f(m['score'])} | {f(p['init_with_fabric'])} | {f(p['remove'])} | "
                 f"{f(p['commit'])} | {f(m['slope_with_fabric_s_per_cycle'])} | {mm('init_with_fabric_s')} | {mm('remove_s')} | {f(m['median_init_before_s'])} | {f(m['median_window_open_s'])} | "
                 f"{f(ph['pase'], 1)} / {f(ph['attestation'], 1)} / {f(ph['csr'], 1)} | {f(ph['noc_to_done'], 1)} | "
                 f"{ms('load_one (hit)')} / {ms('load_one (miss)')} | {ms('val_len (hit)')} / {ms('val_len (miss)')} | {ms('scan (all)')} | "
                 f"{m['commissioning_ok']}/{m['commissioning_total']} | {m['persistence']} | {decision} | {note} |")

lines += ["", "Per-cycle Matter init with fabric, s:", "", "| # | " + " | ".join(f"c{i}" for i in range(1, 11)) + " |",
          "|---|" + "---|" * 10]
for d, m in trials:
    vals = [c["init_with_fabric_s"] for c in m["cycles"]]
    lines.append(f"| {d.name} | " + " | ".join(f(v, 1) for v in vals + [None] * (10 - len(vals))) + " |")
(root / "results.md").write_text("\n".join(lines) + "\n")
print(f"{root / 'results.md'}: {len(trials)} trials, reference {f(reference)}, noise {f(noise)}")
