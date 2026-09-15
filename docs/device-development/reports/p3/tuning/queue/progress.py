"""Progress bar of the tuning queue."""
import re, subprocess, time
from pathlib import Path
S = Path(__file__).resolve().parent
T = Path("/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning")
MIN = {"zms": 9, "file": 22}
q, seen = [], set()
for qf in ("queue1.txt", "queue2.txt", "queue3.txt", "queue4.txt", "queue5.txt", "queue6.txt", "queue7.txt", "queue8.txt", "queue9.txt"):
    if (S / qf).exists():
        for l in (S / qf).read_text().splitlines():
            x = l.split("|")
            if l.strip() and (x[0], x[1]) not in seen:
                seen.add((x[0], x[1])); q.append(x)
done = {(x[0], x[1]) for x in (l.split("|") for l in ["|".join(y) for y in q]) if (T / x[0] / x[1] / "metrics.json").exists()}
alive = subprocess.run(["pgrep", "-f", r"^/bin/zsh /.*/(run_queue2|run_queue3|zms_finish|file_finish|fabrics_chain|rebuild_chain|diag_chain|icache_chain|net_chain|fabrics_polled_chain|prod_verify_chain|prod_interrupt_chain|p256m_chain)\.sh"], capture_output=True).returncode == 0
trial_alive = subprocess.run(["pgrep", "-f", r"[Pp]ython.* /.*/run_(trial|fabrics)\.py --backend"], capture_output=True).returncode == 0
FAB_MIN = {"zms": 20, "file": 60}
fq = [l.split("|") for l in (S / "queue-fabrics2.txt").read_text().splitlines() if l.strip()] if (S / "queue-fabrics2.txt").exists() else []
fdone = [x for x in fq if (T / "fabrics" / f"{x[0]}-{x[1]}" / "metrics.json").exists()]
log = (S / "logs/queue1.out").read_text().splitlines()
cur = next((l for l in reversed(log) if l.startswith("===")), "")

def bar(n, total, w=30):
    f = int(w * n / total) if total else 0
    return "█" * f + "░" * (w - f) + f" {n}/{total} ({100 * n // max(total, 1)}%)"

print(time.strftime("%H:%M"), "очередь", "жива" if alive else "НЕ РАБОТАЕТ", "| run_trial", "идёт" if trial_alive else "нет")
print(f"{'всего':6} {bar(len([x for x in q if (x[0], x[1]) in done]), len(q))}")
left = 0
for b in ("zms", "file"):
    items = [x for x in q if x[0] == b]
    nd = len([x for x in items if (x[0], x[1]) in done])
    left += (len(items) - nd) * MIN[b]
    print(f"{b:6} {bar(nd, len(items))}")
if fq:
    print(f"{'5 fab.':6} {bar(len(fdone), len(fq))}")
    left += sum(FAB_MIN[x[0]] for x in fq if x not in fdone)
print("сейчас:", cur.replace("=== ", "") or "—")
print(f"осталось ≈ {left // 60} ч {left % 60} мин (ZMS {MIN['zms']} мин, File {MIN['file']} мин на прогон; 5 fabrics: ZMS {FAB_MIN['zms']}, File {FAB_MIN['file']} мин, оценка)")
fr = T / "fabrics" / "results.md"
if fr.exists():
    print("\n5 fabrics:")
    for l in [l for l in fr.read_text().splitlines() if l.startswith("| zms-") or l.startswith("| file-")]:
        c = [x.strip() for x in l.strip("|").split("|")]
        print(f"  {c[0]:22} comm {c[1]:>5}  старт k=5 р1/р2 {c[3].split(' / ')[-1]:>6}/{c[4].split(' / ')[-1]:>6}  удаление {c[7]:>13}  ошибок {c[11]}  оценка {c[14]:>6}  годен {c[15]}")
for b in ("zms", "file"):
    r = T / b / "results.md"
    if not r.exists():
        continue
    txt = r.read_text()
    ref = re.search(r"Reference = .*", txt)
    rows = [l for l in txt.splitlines() if l.startswith("| 0")]
    rows = rows[: len(rows) // 2] if rows else rows  # first table only
    print(f"\n{b}: {ref.group(0) if ref else ''}")
    for l in rows[-4:]:
        c = [x.strip() for x in l.strip("|").split("|")]
        print(f"  {c[0]:32} {c[2][:44]:44} score {c[3]:>5}  withF {c[4]:>5} rem {c[5]:>5} commit {c[6]:>5} slope {c[7]:>5}  withF mean/max {c[8]:>11}  rem mean/max {c[9]:>11}  hit/miss {c[14]:>13}  {c[19]}")
