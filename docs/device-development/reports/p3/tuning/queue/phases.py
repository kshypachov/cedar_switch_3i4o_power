import json,statistics as st,sys
T="/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning/fabrics"
for n in sys.argv[1:]:
    try: com=[json.loads(l) for l in open(f"{T}/{n}/run.jsonl")]
    except Exception as e: print(n, e); continue
    com=[e for e in com if e["event"]=="commission"]
    ph={}
    for e in com:
        for k,v in (e["phases"] or {}).items(): ph.setdefault(k,[]).append(v)
    ok=sum(e["ok"] for e in com)
    print(f"{n:18s} n={len(com):2d} ok={ok:2d} total {st.median([e['commission_s'] for e in com]):5.1f} "+" ".join(f"{k} {st.median(v):.2f}" for k,v in ph.items()))
