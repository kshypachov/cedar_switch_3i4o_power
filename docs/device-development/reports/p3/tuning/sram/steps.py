import gzip,re,sys,statistics as st,collections
T="/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning/fabrics"
PAIRS={"PBKDF (0x20→0x21)":("20","21"),"PASE Pake1→Pake2 (0x22→0x23)":("22","23"),"PASE Pake3→Status (0x24→0x40)":("24","40"),
       "CASE Sigma1→Sigma2 (0x30→0x31)":("30","31"),"CASE Sigma3→Status (0x32→0x40)":("32","40")}
def parse(name):
    txt=gzip.open(f"{T}/{name}/chip-tool.log.gz").read().decode("latin-1")
    runs=[]; cur=None
    for line in txt.splitlines():
        m=re.search(r'\[(\d+\.\d+)\]',line)
        if not m: continue
        ts=float(m.group(1))
        mt=re.search(r'Msg (TX|RX) .*?Type 0000:([0-9a-fA-F]{2})',line)
        if mt and mt.group(1)=="TX" and mt.group(2)=="20":
            if cur is None or "PBKDF (0x20→0x21)" in cur["d"]:
                cur={"d":{}, "open":{}, "steps":{}}; runs.append(cur)
        if cur is None: continue
        if mt:
            d,t=mt.group(1),mt.group(2).lower()
            if d=="TX":
                for k,(a,b) in PAIRS.items():
                    if t==a and k not in cur["d"] and k not in cur["open"]: cur["open"][k]=ts
            else:
                for k,(a,b) in PAIRS.items():
                    if t==b and k in cur["open"] and k not in cur["d"]:
                        cur["d"][k]=ts-cur["open"].pop(k)
        s=re.search(r"Performing next commissioning step '(\w+)'",line)
        if s: cur["steps"].setdefault(s.group(1),[ts,None])
        s=re.search(r"Successfully finished commissioning step '(\w+)'",line)
        if s and s.group(1) in cur["steps"] and cur["steps"][s.group(1)][1] is None: cur["steps"][s.group(1)][1]=ts
    return runs
res={}
for n in sys.argv[1:]:
    runs=[r for r in parse(n) if "PASE Pake1→Pake2 (0x22→0x23)" in r["d"]]
    agg=collections.defaultdict(list)
    for r in runs:
        for k,v in r["d"].items(): agg[k].append(v)
        for k,(a,b) in r["steps"].items():
            if b is not None: agg["step "+k].append(b-a)
    res[n]=(len(runs),{k:st.median(v) for k,v in agg.items()})
names=sys.argv[1:]
keys=sorted(res[names[-1]][1], key=lambda k:-res[names[-1]][1][k])
print(f"{'':42s}"+"".join(f"{n:>14s}" for n in names)); print(f"{'commissionings':42s}"+"".join(f"{res[n][0]:14d}" for n in names))
for k in keys:
    v=res[names[-1]][1][k]
    if v<0.05: continue
    print(f"{k:42s}"+"".join(f"{res[n][1].get(k,float('nan')):14.2f}" for n in names))
