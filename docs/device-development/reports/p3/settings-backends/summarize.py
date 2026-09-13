"""Summarise <backend>.jsonl files of run_backend.py into comparison tables.

usage: summarize.py [backend ...]   (default: file zms nvs fcb)
"""
import json, statistics, sys
from pathlib import Path

HERE = Path(__file__).parent
backends = sys.argv[1:] or ["file", "zms", "nvs", "fcb"]

def load(b):
    p = HERE / f"{b}.jsonl"
    return [json.loads(l) for l in p.read_text().splitlines() if l.strip()] if p.exists() else []

def fmt(v, nd=1):
    return "—" if v is None else (f"{v:.{nd}f}" if isinstance(v, float) else str(v))

print("## Summary\n")
print("| Backend | wipe | clean boot Matter init, s | read back | commissionings ok | Matter init before commissioning, s (first / last / max) | Matter init with fabric, s (first / last / max) | commissioning, s (median / max) | fabric removal, s (median / max) | persistence |")
print("|---|---|---|---|---|---|---|---|---|---|")
detail = {}
for b in backends:
    ev = load(b)
    if not ev:
        print(f"| {b} | not run | | | | | | | | |"); continue
    wipe = next((e for e in ev if e["event"] == "wipe"), {})
    boots = [e for e in ev if e["event"] == "boot"]
    clean = next((e for e in boots if e["label"] == "clean partition"), {})
    back = next((e for e in ev if e["event"] == "structure_read_back"), {})
    before = [e.get("matter_init_s") for e in boots if "before commissioning" in e["label"]]
    withf = [e.get("matter_init_s") for e in boots if "with fabric" in e["label"]]
    comm = [e for e in ev if e["event"] == "commission"]
    rem = [e.get("remove_s") for e in ev if e["event"] == "remove_fabrics" and e.get("remove_s") is not None]
    pers = next((e for e in ev if e["event"] == "persistence"), {})
    failed_boots = [e for e in ev if e["event"] == "boot_failed"]
    def trio(xs):
        xs = [x for x in xs if x is not None]
        return "—" if not xs else f"{xs[0]:.1f} / {xs[-1]:.1f} / {max(xs):.1f}"
    ct = [e["commission_s"] for e in comm if e.get("ok")]
    print(f"| {b} | {'ok' if 'rc=0' in (wipe.get('result') or '') else wipe.get('result')} | {fmt(clean.get('matter_init_s'))} | "
          f"{'yes' if back.get('unique_id_same') else 'no'} | {sum(1 for e in comm if e.get('ok'))}/{len(comm)} | {trio(before)} | {trio(withf)} | "
          f"{fmt(statistics.median(ct)) if ct else '—'} / {fmt(max(ct)) if ct else '—'} | "
          f"{fmt(statistics.median(rem)) if rem else '—'} / {fmt(max(rem)) if rem else '—'} | "
          f"{pers.get('reboot_count')} counted / {pers.get('boots_since_wipe', pers.get('boots_in_log'))} boots{'; ' + str(len(failed_boots)) + ' boot failures' if failed_boots else ''} |")
    detail[b] = (boots, comm, ev)

for b, (boots, comm, ev) in detail.items():
    print(f"\n## {b}: per cycle\n")
    print("| cycle | Matter init before, s | window open, s | commissioning | Matter init with fabric, s | removal, s |")
    print("|---|---|---|---|---|---|")
    rems = [e for e in ev if e["event"] == "remove_fabrics"]
    for i in range(1, 11):
        bb = next((e for e in boots if e["label"] == f"cycle {i}: before commissioning"), {})
        bw = next((e for e in boots if e["label"] == f"cycle {i}: with fabric"), {})
        c = comm[i - 1] if i - 1 < len(comm) else {}
        r = rems[i - 1] if i - 1 < len(rems) else {}
        cs = "—" if not c else (f"ok {c.get('commission_s')}" if c.get("ok") else f"FAILED {c.get('commission_s')} {c.get('failed') or c.get('note') or ''}")
        print(f"| {i} | {fmt(bb.get('matter_init_s'))} | {fmt(c.get('window_open_s'))} | {cs} | {fmt(bw.get('matter_init_s'))} | {fmt(r.get('remove_s'))} |")
    bench = next((e for e in ev if e["event"] == "bench"), None)
    if bench:
        print("\nstorage_bench after the cycles:\n```\n" + "\n".join(bench["text"]) + "\n```")
    warn = [w for e in boots for w in e.get("storage_warnings", [])]
    if warn:
        print(f"\nstorage warnings at boot ({len(warn)}), first: `{warn[0][-150:]}`")
