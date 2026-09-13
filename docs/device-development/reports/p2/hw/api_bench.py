#!/usr/bin/env python3
"""P2 hardware checks of web-api / web-auth on the board, from the host (en7).

Each step prints one line and is recorded in results.json. Nothing here
reboots the board; the reboot-persistence check is a separate, agreed step.
"""
import http.client
import json
import secrets
import socket
import statistics
import sys
import threading
import time

BOARD = "192.168.88.14"
V6 = "fe80::8234:28ff:fe10:1273%en7"
V6_HOST = "[fe80::8234:28ff:fe10:1273]"
PASSWORD = "cedar-bench-P2-password"
NEW_PASSWORD = "cedar-bench-P2-changed"
ORIGIN = f"http://{BOARD}"

results = {}


def record(name, **values):
    results[name] = values
    print(f"{name}: " + ", ".join(f"{k}={v}" for k, v in values.items()), flush=True)


def call(method, path, body=None, headers=None, host=BOARD, timeout=30, host_header=None):
    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    data = json.dumps(body).encode() if isinstance(body, (dict, list)) else body
    hdrs = {"Connection": "close"}
    if data is not None and isinstance(body, (dict, list)):
        hdrs["Content-Type"] = "application/json"
    hdrs.update(headers or {})
    t0 = time.perf_counter()
    conn.putrequest(method, path, skip_host=host_header is not None, skip_accept_encoding=True)
    if host_header is not None:
        conn.putheader("Host", host_header)
    for k, v in hdrs.items():
        conn.putheader(k, v)
    if data is not None:
        conn.putheader("Content-Length", str(len(data)))
    conn.endheaders(data)
    resp = conn.getresponse()
    payload = resp.read()
    elapsed = time.perf_counter() - t0
    conn.close()
    return resp.status, {k.lower(): v for k, v in resp.getheaders()}, payload, elapsed


def js(payload):
    try:
        return json.loads(payload)
    except ValueError:
        return None


def pct(samples, p):
    s = sorted(samples)
    rank = max(1, -(-len(s) * p // 100))  # nearest rank
    return s[int(rank) - 1]


def cookie_of(headers):
    return headers["set-cookie"].split(";")[0]


def main():
    status, h, body, dt = call("GET", "/api/v1/auth/state")
    state = js(body)
    record("auth_state_ipv4", status=status, body=state, ms=round(dt * 1000, 1),
           request_id=h.get("x-request-id"), cache=h.get("cache-control"))
    status6, _, body6, dt6 = call("GET", "/api/v1/auth/state", host=V6, host_header=V6_HOST)
    record("auth_state_ipv6_link_local", status=status6, ms=round(dt6 * 1000, 1),
           same_shape=js(body6) is not None and set(js(body6)) == set(state or {}))

    status, h, body, dt = call("GET", "/")
    record("page", status=status, type=h.get("content-type"), csp=h.get("content-security-policy", "")[:40],
           bytes=len(body), ms=round(dt * 1000, 1))

    for method, path in (("GET", "/api/relays/state"), ("POST", "/api/system/reboot"),
                         ("GET", "/api/device/info"), ("POST", "/api/mqtt/settings")):
        status, h, body, _ = call(method, path, body=b"{}" if method == "POST" else None)
        record(f"legacy {method} {path}", status=status, type=h.get("content-type"),
               code=(js(body) or {}).get("error", {}).get("code"))
    try:
        s = socket.create_connection((BOARD, 8080), timeout=3)
        s.close()
        record("legacy_port_8080", open=True)
    except OSError as err:
        record("legacy_port_8080", open=False, error=type(err).__name__)

    status, _, body, _ = call("GET", "/api/v1/auth/state", host_header="rebound.example")
    record("foreign_host", status=status, code=(js(body) or {}).get("error", {}).get("code"),
           leaks_token="setup_token" in body.decode(errors="replace"))

    if state and state.get("setup_allowed"):
        status, h, body, dt = call("POST", "/api/v1/auth/setup", {"password": PASSWORD},
                                   headers={"Origin": ORIGIN, "X-Setup-Token": state["setup_token"]})
        record("setup", status=status, ms=round(dt * 1000, 1), location=h.get("location"),
               cookie_flags=h.get("set-cookie", "").split(";", 1)[-1].strip())
    else:
        record("setup", skipped="setup not open; the board already has an administrator")

    gets = [call("GET", "/api/v1/auth/state")[3] for _ in range(10)]
    logins, cookie, csrf = [], None, None
    for _ in range(5):
        status, h, body, dt = call("POST", "/api/v1/auth/session", {"password": PASSWORD},
                                   headers={"Origin": ORIGIN})
        logins.append(dt)
        if status == 200:
            cookie, csrf = cookie_of(h), js(body)["csrf_token"]
    wrong = [call("POST", "/api/v1/auth/session", {"password": "not the password"})[3] for _ in range(3)]
    record("kdf_latency", get_median_ms=round(statistics.median(gets) * 1000, 1),
           login_median_ms=round(statistics.median(logins) * 1000, 1),
           login_max_ms=round(max(logins) * 1000, 1),
           wrong_median_ms=round(statistics.median(wrong) * 1000, 1),
           derivation_estimate_ms=round((statistics.median(logins) - statistics.median(gets)) * 1000, 1))
    # A success clears this address's count before the rate-limit step.
    status, h, body, _ = call("POST", "/api/v1/auth/session", {"password": PASSWORD})
    cookie, csrf = cookie_of(h), js(body)["csrf_token"]

    samples = []
    statuses = []
    for _ in range(50):
        s, _, _, dt = call("GET", "/api/v1/system/status", headers={"Cookie": cookie})
        samples.append(dt)
        statuses.append(s)
    record("status_sequential_50", ok=statuses.count(200), p50_ms=round(pct(samples, 50) * 1000, 1),
           p95_ms=round(pct(samples, 95) * 1000, 1), max_ms=round(max(samples) * 1000, 1))

    for clients in (2, 4):
        lat, codes, lock = [], [], threading.Lock()

        def worker():
            for _ in range(25):
                try:
                    s, _, _, dt = call("GET", "/api/v1/system/status", headers={"Cookie": cookie}, timeout=30)
                except Exception as err:  # noqa: BLE001 - recorded, not raised
                    s, dt = type(err).__name__, 0
                with lock:
                    codes.append(s)
                    if s == 200:
                        lat.append(dt)

        threads = [threading.Thread(target=worker) for _ in range(clients)]
        t0 = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        record(f"status_{clients}_clients", requests=len(codes), ok=codes.count(200),
               others=sorted({str(c) for c in codes if c != 200}),
               p95_ms=round(pct(lat, 95) * 1000, 1) if lat else None,
               wall_s=round(time.perf_counter() - t0, 1))

    # Abort: a body cut off by close must not block the next client.
    raw = socket.create_connection((BOARD, 80), timeout=5)
    raw.sendall(b"POST /api/v1/auth/session HTTP/1.1\r\nHost: 192.168.88.14\r\n"
                b"Content-Type: application/json\r\nContent-Length: 40\r\n\r\n{\"password\":")
    time.sleep(0.3)
    raw.close()
    time.sleep(0.2)
    status, _, _, dt = call("POST", "/api/v1/auth/session", {"password": "not the password"})
    record("after_abort_same_resource", status=status, ms=round(dt * 1000, 1))
    call("POST", "/api/v1/auth/session", {"password": PASSWORD})  # clear the count again

    # Stall: a body that never finishes holds its resource until the timeout.
    raw = socket.create_connection((BOARD, 80), timeout=30)
    raw.sendall(b"POST /api/v1/auth/session HTTP/1.1\r\nHost: 192.168.88.14\r\n"
                b"Content-Type: application/json\r\nContent-Length: 40\r\n\r\n{\"password\":")
    t0 = time.perf_counter()
    time.sleep(0.3)
    other = call("GET", "/api/v1/system/status", headers={"Cookie": cookie})
    blocked = call("GET", "/api/v1/auth/session", headers={"Cookie": cookie})
    free_at = None
    while time.perf_counter() - t0 < 30:
        s, _, _, _ = call("GET", "/api/v1/auth/session", headers={"Cookie": cookie})
        if s == 200:
            free_at = time.perf_counter() - t0
            break
        time.sleep(0.5)
    raw.close()
    record("stalled_client", other_resource=other[0], same_resource=blocked[0],
           same_resource_body=blocked[2][:40].decode(errors="replace"),
           released_after_s=round(free_at, 1) if free_at else None)

    codes = []
    retry = None
    for _ in range(6):
        s, h, body, _ = call("POST", "/api/v1/auth/session", {"password": "not the password"})
        codes.append(s)
        if s == 429:
            retry = h.get("retry-after")
    record("rate_limit", statuses=codes, retry_after=retry)
    time.sleep(int(retry or 1) + 1)

    status, h, body, _ = call("POST", "/api/v1/auth/session", {"password": PASSWORD})
    cookie, csrf = cookie_of(h), js(body)["csrf_token"]
    key = secrets.token_urlsafe(24)
    t0 = time.perf_counter()
    status, h, body, _ = call("PUT", "/api/v1/auth/password",
                              {"current_password": PASSWORD, "new_password": NEW_PASSWORD},
                              headers={"Cookie": cookie, "X-CSRF-Token": csrf, "Idempotency-Key": key})
    accepted = js(body)
    polls = []
    outcome = None
    while time.perf_counter() - t0 < 30:
        s, _, b, _ = call("GET", f"/api/v1/jobs/{accepted['job_id']}", headers={"Cookie": cookie})
        polls.append(s if s != 200 else js(b)["state"])
        if s == 401:
            outcome = "session_ended"
            break
        if s == 200 and js(b)["state"] in ("succeeded", "failed"):
            outcome = js(b)["state"]
            break
        time.sleep(0.25)
    change_s = time.perf_counter() - t0
    old = call("POST", "/api/v1/auth/session", {"password": PASSWORD})[0]
    new = call("POST", "/api/v1/auth/session", {"password": NEW_PASSWORD})[0]
    record("password_change", accepted=status, outcome=outcome, polls=polls,
           seconds=round(change_s, 2), old_password=old, new_password=new)

    # The new verifier was stored with the build's iteration count.
    after = [call("POST", "/api/v1/auth/session", {"password": NEW_PASSWORD}, timeout=60)[3] for _ in range(5)]
    record("kdf_latency_after_change", login_median_ms=round(statistics.median(after) * 1000, 1),
           login_max_ms=round(max(after) * 1000, 1))

    status, h, body, _ = call("GET", "/assets/" + next(
        (line.split('/assets/')[1].split('"')[0] for line in call("GET", "/")[2].decode().splitlines()
         if 'src="/assets/' in line), "missing.js"), headers={"Accept-Encoding": "gzip"})
    record("asset", status=status, encoding=h.get("content-encoding"), cache=h.get("cache-control"),
           bytes=len(body), etag=h.get("etag"))

    json.dump(results, open(sys.argv[1], "w"), indent=2, default=str)


if __name__ == "__main__":
    main()
