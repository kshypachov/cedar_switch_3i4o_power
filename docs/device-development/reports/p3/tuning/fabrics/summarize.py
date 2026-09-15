"""Metrics of five-fabric runs: fabrics/<backend>-<name>/ -> metrics.json, then fabrics/results.md.

usage: summarize.py <run dir> [...]      (no arguments: only regenerate results.md)

Criteria (frozen in tuning/README.md before the first run): usable = 10/10
commissionings, persistence, no unexpected boots, no storage errors; among
usable candidates lower (max Matter init with 5 fabrics + max removal of 5
fabrics) is better.
"""
import gzip, json, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
STORAGE = re.compile(r"settings|zms|littlefs|fs_srv|spi_nor|flash|\bfs:", re.I)
BENCH = re.compile(r"(load_one \(hit\)|scan \(all\)|open\+close|read 1024)\s+(\d+)\s*/\s*(\d+)")
SIZE = re.compile(r"(?:file \S+: (\d+) bytes, )?(\d+) entries")
ORDER = ["zms-cache2048", "zms-cache2048-sectors8", "zms-cache2048-sectors4", "file-ml128", "file-ml96", "file-ml64", "zms-boardb-prod-sectors4-w5500up", "zms-prod-sectors4", "zms-prod-sectors4-spi_interrupt", "zms-prod-sectors4-p256m", "zms-sram-c0", "zms-sram-s1", "zms-sram-s2", "zms-sram-s3", "zms-sram-s3-p256m", "zms-pka-b-off", "zms-pka-b-on", "zms-pka-c-off", "zms-pka-c-on", "zms-pka-c2-off", "zms-pka-c2-on", "zms-sram-s1-killed", "zms-boardb-pka-c-on"]


def console_text(d):
    gz, raw = d / "console.log.gz", d / "console.raw"
    data = gzip.open(gz).read() if gz.exists() else raw.read_bytes()
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", data.decode("latin-1")).replace("\r", "")


def summarize(d):
    ev = [json.loads(l) for l in (d / "run.jsonl").read_text().splitlines() if l.strip()]
    meta = json.loads((d / "meta.json").read_text())
    boots = [e for e in ev if e["event"] == "boot"]
    comm = [e for e in ev if e["event"] == "commission"]
    rem = [e for e in ev if e["event"] == "remove_fabrics"]
    text = console_text(d)
    # One bootloader line per boot plus the start after flashing.
    unexpected = text.count("Starting bootloader") - 1 - len(boots)
    msgs = {}
    # Storage errors count from the restart after creation (third bootloader line: after
    # flashing, clean partition, restart). The first two boots meet the previous image's data
    # and the erased partition; LittleFS then reports "Corrupted dir pair", formats and
    # "file open error (-2)" before the settings file exists - the method, not a failure.
    parts = text.split("Starting bootloader", 3)
    measured = parts[3] if len(parts) == 4 else text
    for line in measured.splitlines():
        if re.search(r"<err>|<wrn>", line):
            key = re.sub(r"^.*?(<err>|<wrn>)", r"\1", line).strip()
            key = re.sub(r"0x[0-9A-Fa-f]+|\b\d+\b", "#", key)[:140]
            msgs[key] = msgs.get(key, 0) + 1
    storage_msgs = {k: v for k, v in msgs.items() if STORAGE.search(k) and "<err>" in k}
    init = {}
    for b in boots:
        if "round" in b:
            init[f"r{b['round']}_k{b['k']}"] = dict(fabrics=b.get("fabrics"), init_s=b.get("matter_init_s"),
                                                   zms=[z for z in b.get("zms", []) if "alloc wra" in z or "GC" in z or "gc" in z])
    bench = {}
    for e in ev:
        if e["event"] == "bench":
            r = {}
            for l in e.get("text", []):
                m = BENCH.search(l)
                if m:
                    r[m[1]] = int(m[3])
            m = next((SIZE.search(l) for l in e.get("text", []) if "entries" in l), None)
            if m:
                r["bytes"] = int(m[1]) if m[1] else None
                r["entries"] = int(m[2])
            bench[e["label"]] = r
    with5 = [b.get("matter_init_s") for b in boots if b.get("fabrics") == 5]
    removes = [r["remove_s"] for r in rem if r.get("remove_s") is not None and (r.get("before") or 0) > 0]
    pers = next((e for e in ev if e["event"] == "persistence"), {})
    back = next((e for e in ev if e["event"] == "structure_read_back"), {})
    ok = sum(1 for c in comm if c.get("ok"))
    max_fabrics = max([b.get("fabrics") or 0 for b in boots], default=0)
    usable = (ok == 10 and len(comm) == 10 and bool(pers.get("ok")) and unexpected == 0 and not storage_msgs
              and max_fabrics == 5)
    m = dict(meta=meta, aborted=next((e["reason"] for e in ev if e["event"] == "aborted"), None),
             commissioning_ok=ok, commissioning_total=len(comm), max_fabrics=max_fabrics,
             max_init_with_5_s=max(with5) if with5 else None, max_remove_s=max(removes) if removes else None,
             score=round(max(with5) + max(removes), 2) if with5 and removes else None, usable=usable,
             read_back_ok=bool(back.get("unique_id_same") and back.get("counter_advanced")),
             persistence_ok=pers.get("ok"), persistence=f"{pers.get('reboot_count')} / {pers.get('boots_since_wipe')}" if pers else None,
             unexpected_boots=unexpected, boot_failures=sum(1 for e in ev if e["event"] == "boot_failed"),
             init=init,
             commissions=[{k: c.get(k) for k in ("round", "k", "commissioner", "ok", "window_open_s", "commission_s",
                                                  "failed", "phases", "device_errors")} for c in comm],
             removals=[{k: r.get(k) for k in ("round", "attempt", "before", "after", "remove_s")} for r in rem],
             bench=bench, storage_errors=storage_msgs,
             other_warnings=dict(sorted(((k, v) for k, v in msgs.items() if k not in storage_msgs),
                                        key=lambda kv: -kv[1])[:15]))
    (d / "metrics.json").write_text(json.dumps(m, indent=1, ensure_ascii=False) + "\n")
    return m


def fmt(x, nd=1):
    return "—" if x is None else f"{x:.{nd}f}".replace(".", ",")


def results():
    rows = []
    runs = sorted((p for p in HERE.iterdir() if (p / "metrics.json").exists()),
                  key=lambda p: (ORDER.index(p.name) if p.name in ORDER else 99, p.name))
    for p in runs:
        m = json.loads((p / "metrics.json").read_text())
        ini = m["init"]
        seq = lambda r: " / ".join(fmt(ini.get(f"r{r}_k{k}", {}).get("init_s")) for k in range(1, 6))
        comm = {(c["round"], c["k"]): c for c in m["commissions"]}
        c5 = lambda r: comm.get((r, 5), {})
        commits = [c["phases"].get("commit") for c in m["commissions"] if c.get("phases")]
        commits = [x for x in commits if x is not None]
        rem = {r["round"]: r["remove_s"] for r in m["removals"] if (r.get("before") or 0) > 0}
        b = m["bench"].get("round 2: five fabrics") or m["bench"].get("round 1: five fabrics") or {}
        size = f"{b.get('bytes')} Б, {b.get('entries')} зап." if b.get("bytes") else (f"{b.get('entries')} зап." if b else "—")
        zms_last = next((z for z in reversed([z for v in ini.values() for z in v.get("zms", []) if "alloc wra" in z])), "")
        note = (p / "notes.md").read_text().splitlines()[0] if (p / "notes.md").exists() else ""
        rows.append(f"| {p.name} | {m['commissioning_ok']}/{m['commissioning_total']} | {m['max_fabrics']} | {seq(1)} | {seq(2)} "
                    f"| {fmt(c5(1).get('commission_s'))} / {fmt(c5(2).get('commission_s'))} | {fmt(max(commits) if commits else None, 2)} "
                    f"| {fmt(rem.get(1))} / {fmt(rem.get(2))} | {fmt((b.get('load_one (hit)') or 0) / 1000 if b else None)} | {size} "
                    f"| {zms_last or '—'} | {len(m['storage_errors'])} | {m['persistence'] or '—'} | {m['unexpected_boots']} "
                    f"| {fmt(m['score'])} | {'да' if m['usable'] else 'нет'} | {note} |")
    head = ("# Проверка на 5 fabrics\n\nМетодика и критерий — tuning/README.md, раздел «Проверка лучших кандидатов на 5 fabrics». "
            "Время в секундах. Старт — инициализация Matter после перезагрузки с k fabrics (k = 1…5), раунды 1 и 2. "
            "Оценка = макс. старт с 5 fabrics + макс. удаление 5 fabrics (меньше — лучше), сравнивается только у годных.\n\n"
            "| Кандидат | Commissioning | Макс. fabrics | Старт k=1…5, раунд 1 | Старт k=1…5, раунд 2 | Commissioning 5-го, р1 / р2 "
            "| Макс. commit | Удаление 5, р1 / р2 | load_one, мс | Объём при 5 | ZMS alloc wra (последний) | Ошибок хранилища "
            "| Сохранность | Лишние загрузки | Оценка | Годен | Заметка |\n"
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
    (HERE / "results.md").write_text(head + "\n".join(rows) + "\n")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        m = summarize(Path(arg))
        print(f"{arg}: comm {m['commissioning_ok']}/{m['commissioning_total']} max fabrics {m['max_fabrics']} "
              f"init5 {m['max_init_with_5_s']} remove {m['max_remove_s']} score {m['score']} usable {m['usable']} "
              f"storage errors {len(m['storage_errors'])} persistence {m['persistence']} aborted {m['aborted']}")
    results()
