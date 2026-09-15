import gzip,re,sys,statistics as st,collections
T="/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning/fabrics"
def ts(l):
    m=re.search(r'\[(\d+):(\d+):(\d+)\.(\d+),',l); return int(m.group(1))*3600+int(m.group(2))*60+int(m.group(3))+int(m.group(4))/1000 if m else None
MARK={"sess":"Commissioning session establishment step started","pake1":"Type 0000:22","att_rx":"Received an AttestationRequest","att_ok":"AttestationRequest successful",
 "csr_rx":"Received a CSRRequest","csr_ok":"CSRRequest successful","root_rx":"Received an AddTrustedRootCertificate","root_ok":"AddTrustedRootCertificate successful",
 "noc_v":"Validating NOC chain","noc_vok":"NOC chain validation successful","noc_ok":"successfully created fabric index","s1":"Received Sigma1 msg","s2":"Sent Sigma2 msg",
 "s3":"Received Sigma3 msg","case_ok":"CASE Session established","cc":"Received CommissioningComplete","done":"Commissioning completed successfully"}
PH=[("PASE: SPAKE2+ (Pake1→Pake2)","pake1","pake2"),("CASE Sigma3: проверка NOC-цепочки и подписи (Sigma3→сессия)","s3","case_ok"),
    ("AddNOC: проверка NOC-цепочки","noc_v","noc_vok"),("CASE Sigma2: ключ, ECDH, подпись (Sigma1→Sigma2)","s1","s2"),
    ("AddTrustedRoot: проверка RCAC","root_rx","root_ok"),("CSR: генерация ключа + подпись","csr_rx","csr_ok"),
    ("Attestation: подпись DAC","att_rx","att_ok"),("AddNOC: запись fabric, ACL","noc_vok","noc_ok"),("всё сопряжение на плате","sess","done")]
for name in sys.argv[1:]:
    txt=gzip.open(f"{T}/{name}/console.log.gz").read().decode("latin-1").replace("\r","")
    txt=re.sub(r"\x1b\[[0-9;]*[A-Za-z]","",txt)
    runs=[]; cur=None
    for l in txt.splitlines():
        t=ts(l)
        if t is None: continue
        if MARK["sess"] in l: cur={"sess":t}; runs.append(cur); continue
        if cur is None: continue
        # Pake2 is the first TX well after Pake1: the standalone ack goes out within ~10-20 ms, Pake2
        # takes >= 0.3 s even on the PKA (0.5 s here cut off the PKA runs, 2026-09-15 pka-c-on)
        if "pake1" in cur and "pake2" not in cur and "Msg TX" in l and t - cur["pake1"] > 0.15: cur["pake2"]=t
        for k,s in MARK.items():
            if k!="sess" and s in l and k not in cur: cur[k]=t
    runs=[r for r in runs if "done" in r]
    print(f"== {name}: {len(runs)} commissionings")
    for label,a,b in PH:
        v=[r[b]-r[a] for r in runs if a in r and b in r]
        print(f"  {label:62s} {st.median(v):6.2f} (min {min(v):.2f} max {max(v):.2f})")
