"""Board B only: first-time setup through the API — read the setup token and set the bench password.

A copy of reports/p3/hw/setup_admin.py with board B's address; board A is another
session's and must not be sent anything.

usage: setup_admin_b.py   (prints the result; does nothing if setup is closed)
"""
import http.client, json, sys, time

HOST = "192.168.88.13"
PASSWORD = "cedar-bench-P4-boardB"

def call(method, path, body=None, headers=None):
    h = {"Host": HOST, "Origin": f"http://{HOST}", "Content-Type": "application/json"}
    h.update(headers or {})
    c = http.client.HTTPConnection(HOST, 80, timeout=30)
    c.request(method, "/api/v1" + path, body=json.dumps(body) if body is not None else None, headers=h)
    r = c.getresponse(); raw = r.read(); c.close()
    return r.status, json.loads(raw) if raw else None

for _ in range(60):
    try:
        s, state = call("GET", "/auth/state")
        break
    except OSError:
        time.sleep(2)
else:
    sys.exit("device not answering")
print("auth state", s, state)
if not state.get("setup_allowed"):
    print("setup closed; nothing to do")
    sys.exit(0)
t0 = time.monotonic()
s, body = call("POST", "/auth/setup", {"password": PASSWORD}, {"X-Setup-Token": state["setup_token"]})
print("setup", s, f"{time.monotonic() - t0:.2f}s", "session" if s == 201 else body)
