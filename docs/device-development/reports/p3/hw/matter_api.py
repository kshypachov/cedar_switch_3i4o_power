"""P3 board check of the Matter API: reads, window job, codes, fabrics, latency.

usage: matter_api.py <out.json> [phase]
  phase "reads"  - sign in, read every Matter resource and capabilities, time them
  phase "open"   - open a 180 s window through the API, poll its job, read the codes
  phase "close"  - close the window through the API, poll its job
  phase "fabrics"- list fabrics
Every response body is validated against openapi.json.
"""
import http.client, json, re, sys, time, uuid
from pathlib import Path

HOST = "192.168.88.14"
PASSWORD = "cedar-bench-P2-changed"
DOC = json.loads(Path("/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/openapi.json").read_text())
sys.path.insert(0, "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tools/api-contract")
import jsonschema
from jsonschema import Draft202012Validator
RES = {"$id": "doc", "components": DOC["components"]}
def validate(schema, body):
    v = Draft202012Validator({"$ref": f"doc#/components/schemas/{schema}"},
                             registry=__import__("referencing").Registry().with_resource("doc", __import__("referencing").Resource.from_contents(RES, default_specification=__import__("referencing.jsonschema").jsonschema.DRAFT202012)))
    errors = [e.message for e in v.iter_errors(body)]
    return errors

state = {"cookie": None, "csrf": None}
def call(method, path, body=None, headers=None, timeout=30):
    h = {"Host": HOST}
    if state["cookie"]:
        h["Cookie"] = state["cookie"]
    if headers:
        h.update(headers)
    data = None
    if body is not None:
        data = json.dumps(body).encode(); h["Content-Type"] = "application/json"
    c = http.client.HTTPConnection(HOST, 80, timeout=timeout)
    t0 = time.monotonic()
    c.request(method, "/api/v1" + path, body=data, headers=h)
    r = c.getresponse(); raw = r.read(); dt = time.monotonic() - t0
    hdrs = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    try:
        parsed = json.loads(raw) if raw else None
    except ValueError:
        parsed = raw.decode(errors="replace")
    return r.status, hdrs, parsed, dt

def login():
    s, h, b, dt = call("POST", "/auth/session", {"password": PASSWORD}, {"Origin": f"http://{HOST}"})
    assert s == 200, (s, b)
    state["cookie"] = h["set-cookie"].split(";")[0]
    state["csrf"] = b["csrf_token"]
    return dt

def poll(job_id, limit=60):
    t0 = time.monotonic()
    while True:
        s, h, b, dt = call("GET", f"/jobs/{job_id}")
        assert s == 200, (s, b)
        if b["state"] in ("succeeded", "failed", "cancelled", "interrupted"):
            return b, time.monotonic() - t0
        if time.monotonic() - t0 > limit:
            return b, time.monotonic() - t0
        time.sleep(0.5)

out = {"phase": sys.argv[2] if len(sys.argv) > 2 else "reads", "when": time.strftime("%Y-%m-%dT%H:%M:%S")}
out["login_s"] = round(login(), 3)
phase = out["phase"]
reads = [("GET", "/matter/status", "MatterStatus"), ("GET", "/matter/commissioning", "CommissioningWindow"),
         ("GET", "/matter/onboarding-codes", "OnboardingCodes"), ("GET", "/matter/fabrics", "Fabrics"),
         ("GET", "/capabilities", "Capabilities")]
if phase in ("reads", "fabrics"):
    out["reads"] = {}
    for m, p, schema in reads:
        times = []
        for _ in range(5 if phase == "reads" else 1):
            s, h, b, dt = call(m, p); times.append(dt)
        out["reads"][p] = {"status": s, "schema_errors": validate(schema, b), "body": b,
                           "p50_ms": round(sorted(times)[len(times)//2]*1000), "max_ms": round(max(times)*1000)}
if phase == "open":
    key = "p3-open-" + uuid.uuid4().hex[:16]
    s, h, b, dt = call("POST", "/matter/commissioning", {"mode": "basic", "timeout_seconds": 180},
                       {"X-CSRF-Token": state["csrf"], "Idempotency-Key": key})
    out["open"] = {"status": s, "body": b, "ms": round(dt*1000), "location": h.get("location")}
    if s == 202:
        job, waited = poll(b["job_id"])
        out["open"]["job"] = job; out["open"]["job_s"] = round(waited, 2)
        s2, _, b2, _ = call("POST", "/matter/commissioning", {"mode": "basic", "timeout_seconds": 180},
                            {"X-CSRF-Token": state["csrf"], "Idempotency-Key": key})
        out["open"]["retry_same_key"] = {"status": s2, "job_id": (b2 or {}).get("job_id")}
        s3, _, b3, _ = call("POST", "/matter/commissioning", {"mode": "basic", "timeout_seconds": 180},
                            {"X-CSRF-Token": state["csrf"], "Idempotency-Key": key + "x"})
        out["open"]["second_open"] = {"status": s3, "code": ((b3 or {}).get("error") or {}).get("code")}
    for m, p, schema in reads[1:3]:
        s, h, b, dt = call(m, p)
        out.setdefault("after_open", {})[p] = {"status": s, "schema_errors": validate(schema, b), "body": b, "ms": round(dt*1000)}
if phase == "close":
    key = "p3-close-" + uuid.uuid4().hex[:16]
    s, h, b, dt = call("DELETE", "/matter/commissioning", None, {"X-CSRF-Token": state["csrf"], "Idempotency-Key": key})
    out["close"] = {"status": s, "body": b, "ms": round(dt*1000)}
    if s == 202:
        job, waited = poll(b["job_id"]); out["close"]["job"] = job; out["close"]["job_s"] = round(waited, 2)
    for m, p, schema in reads[1:3]:
        s, h, b, dt = call(m, p)
        out.setdefault("after_close", {})[p] = {"status": s, "schema_errors": validate(schema, b), "body": b}
Path(sys.argv[1]).write_text(json.dumps(out, indent=2, ensure_ascii=False))
print(json.dumps(out, indent=1, ensure_ascii=False)[:6000])
