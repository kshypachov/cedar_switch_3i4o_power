"""Open a window through the API, sign in while the Matter thread is busy, and
snapshot the kernel's thread list while the sign-in waits.

usage: stall_capture.py <out-prefix>
"""
import http.client, json, re, socket, sys, threading, time, uuid

HOST = "192.168.88.14"
prefix = sys.argv[1]
H = {"Host": HOST, "Origin": f"http://{HOST}", "Content-Type": "application/json"}

def request(method, path, body=None, headers=None, timeout=30):
    c = http.client.HTTPConnection(HOST, 80, timeout=timeout)
    h = dict(H); h.update(headers or {})
    t0 = time.monotonic()
    c.request(method, "/api/v1" + path, body=json.dumps(body) if body is not None else None, headers=h)
    r = c.getresponse(); raw = r.read(); hdrs = {k.lower(): v for k, v in r.getheaders()}; c.close()
    return r.status, hdrs, (json.loads(raw) if raw else None), time.monotonic() - t0

def shell(cmd):
    s = socket.create_connection((HOST, 23), timeout=5); s.settimeout(0.3)
    time.sleep(0.4)
    try: s.recv(65536)
    except socket.timeout: pass
    s.sendall(cmd.encode() + b"\r\n")
    buf = b""; end = time.monotonic() + 4; last = time.monotonic()
    while time.monotonic() < end:
        try:
            d = s.recv(65536)
            if d: buf += d; last = time.monotonic()
        except socket.timeout:
            if buf and time.monotonic() - last > 0.8: break
    s.close()
    return re.sub(rb"\x1b\[[0-9;]*[A-Za-z]|\xff[\xfb-\xfe].|\r", b"", buf).decode(errors="replace")

log = open(prefix + ".out", "w")
def note(text):
    line = f"{time.strftime('%H:%M:%S')} {text}"; print(line, flush=True); log.write(line + "\n"); log.flush()

s, h, b, dt = request("POST", "/auth/session", {"password": "cedar-bench-P2-changed"})
note(f"idle sign-in {s} {dt:.2f}s")
cookie = h["set-cookie"].split(";")[0]; csrf = b["csrf_token"]
auth = {"Cookie": cookie, "X-CSRF-Token": csrf}
note("thread list at rest:\n" + shell("kernel thread list"))

s, h, b, dt = request("POST", "/matter/commissioning", {"mode": "basic", "timeout_seconds": 180},
                      dict(auth, **{"Idempotency-Key": "stall-" + uuid.uuid4().hex[:16]}))
note(f"open window {s} {dt*1000:.0f}ms job {b and b.get('job_id')}")
result = {}
def slow_login():
    try:
        result["login"] = request("POST", "/auth/session", {"password": "cedar-bench-P2-changed"}, timeout=40)[::3]
    except Exception as e:
        result["login"] = (f"FAILED {type(e).__name__}",)
t = threading.Thread(target=slow_login); t.start()
for i in range(4):
    time.sleep(1.5)
    if not t.is_alive():
        break
    note(f"thread list while sign-in waits ({i}):\n" + shell("kernel thread list"))
t.join()
note(f"sign-in during window open: {result.get('login')}")
job = b and b.get("job_id")
for _ in range(40):
    s, _, jb, _ = request("GET", f"/jobs/{job}", headers={"Cookie": cookie})
    if jb and jb.get("state") in ("succeeded", "failed"):
        note(f"open job {jb['state']}"); break
    time.sleep(1)
s, _, b, _ = request("DELETE", "/matter/commissioning", None, dict(auth, **{"Idempotency-Key": "stall-close-" + uuid.uuid4().hex[:12]}))
note(f"close {s}")
