"""Board B only: open a commissioning window through board B's API, check its mDNS
advertisement from the host, close the window.

The P4 question is whether Matter discovery survives the network changes: after DHCP,
static and back, the commissionable record must resolve to board B's host name and an
address board B holds. Browsing is passive; nothing is sent to any other node.

usage: matter_mdns_b.py <out.json>
"""
import http.client, json, re, subprocess, sys, time, uuid
from pathlib import Path

HOST = "192.168.88.13"
PASSWORD = "cedar-bench-P4-boardB"
BOARD_B_MAC_HOST = "8034281" "06A1D"   # the Matter host name is built from the MAC
out = {"when": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "steps": []}

def step(name, **kw):
    out["steps"].append({"step": name, **kw}); print(name, json.dumps(kw, ensure_ascii=False)[:600], flush=True)

state = {}
def call(method, path, body=None, headers=None):
    h = {"Host": HOST, "Origin": f"http://{HOST}"}
    if "cookie" in state:
        h["Cookie"] = state["cookie"]
    if "csrf" in state and method != "GET":
        h["X-CSRF-Token"] = state["csrf"]
    h.update(headers or {})
    data = json.dumps(body).encode() if body is not None else None
    if data:
        h["Content-Type"] = "application/json"
    c = http.client.HTTPConnection(HOST, 80, timeout=30)
    c.request(method, "/api/v1" + path, body=data, headers=h)
    r = c.getresponse(); raw = r.read(); hdrs = {k.lower(): v for k, v in r.getheaders()}; c.close()
    return r.status, hdrs, (json.loads(raw) if raw else None)

def dns_sd(args, seconds):
    # Through a pty: dns-sd block-buffers into a pipe and loses its output when stopped.
    p = subprocess.Popen(["script", "-q", "/dev/null", "dns-sd", *args], stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(seconds); p.terminate()
    return p.communicate()[0].replace("\r", "")

def poll_job(job_id, limit=60):
    t = time.monotonic()
    while time.monotonic() - t < limit:
        s, _, b = call("GET", f"/jobs/{job_id}")
        if s != 200 or b["state"] in ("succeeded", "failed", "cancelled", "interrupted"):
            return b, round(time.monotonic() - t, 2)
        time.sleep(0.5)
    return b, round(time.monotonic() - t, 2)

s, h, b = call("POST", "/auth/session", {"password": PASSWORD})
state["cookie"] = h["set-cookie"].split(";")[0]; state["csrf"] = b["csrf_token"]
s, _, st = call("GET", "/network/status"); step("network status", status=s, body=st)
s, _, b = call("POST", "/matter/commissioning", {"mode": "basic", "timeout_seconds": 180},
               {"Idempotency-Key": "p4-" + uuid.uuid4().hex[:16]})
step("open window", status=s, body=b)
if s == 202:
    job, waited = poll_job(b["job_id"])
    step("open job", job=job, waited_s=waited)
browse = dns_sd(["-B", "_matterc._udp", "local."], 8)
instances = sorted(set(re.findall(r"_matterc\._udp\.\s+(\S+)\s*$", browse, re.M)))
step("browse _matterc._udp", instances=instances)
found = []
for inst in instances:
    res = dns_sd(["-L", inst, "_matterc._udp", "local."], 3)
    m = re.search(r"can be reached at (\S+?)\.?:(\d+)", res)
    if m and BOARD_B_MAC_HOST in m.group(1).upper():
        addrs = dns_sd(["-G", "v4v6", m.group(1)], 5)
        found.append({"instance": inst, "host": m.group(1), "port": int(m.group(2)),
                      "addresses": sorted(set(re.findall(r"\s((?:[0-9a-fA-F]{0,4}:){2,7}[0-9a-fA-F]{0,4}(?:%\S+)?|\d+\.\d+\.\d+\.\d+)\s", addrs + " "))),
                      "resolve_raw": res.strip()[-600:], "address_raw": addrs.strip()[-900:]})
step("board B record", records=found)
s, _, b = call("DELETE", "/matter/commissioning", None, {"Idempotency-Key": "p4-" + uuid.uuid4().hex[:16]})
step("close window", status=s, body=b)
if s == 202:
    job, waited = poll_job(b["job_id"])
    step("close job", job=job, waited_s=waited)
Path(sys.argv[1]).write_text(json.dumps(out, indent=2, ensure_ascii=False))
