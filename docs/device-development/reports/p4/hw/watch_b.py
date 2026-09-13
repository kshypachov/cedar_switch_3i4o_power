"""Board B only: record its console and poll its network status for a while, on one host
clock, without resetting it. For the owner's physical scenarios (cable, power): the
owner acts, this records what the board said and what it answered.

Every console line carries host seconds since start; every poll is one line too:
    [+  12.34] ==== poll ping=ok http=200 eth=ready link=True v4=192.168.88.13 dns=[...]
A board that loses power stops answering and its console restarts at 00:00:00; both
land on the same timeline.

usage: watch_b.py <out.log> <seconds> [--poll-every 2]
"""
import http.client, json, subprocess, sys, threading, time

HOST = "192.168.88.13"
PASSWORD = "cedar-bench-P4-boardB"
UART = "/dev/cu.usbmodem5AE60208891"

OUT, SECONDS = sys.argv[1], float(sys.argv[2])
EVERY = float(sys.argv[sys.argv.index("--poll-every") + 1]) if "--poll-every" in sys.argv else 2.0
T0 = time.time()
lines, lock, stop = [], threading.Lock(), threading.Event()

def add(text):
    with lock:
        lines.append(f"[+{time.time() - T0:8.2f}] {text}")

def console():
    import re, serial
    while not stop.is_set():
        try:
            s = serial.Serial(); s.port, s.baudrate, s.timeout = UART, 115200, 0.2
            s.dtr = s.rts = False; s.open()
        except Exception as e:           # USB serial of the adapter, not the board: stays up
            add(f"==== console open failed: {e}"); time.sleep(1); continue
        partial = ""
        try:
            while not stop.is_set():
                chunk = s.read(4096)
                if not chunk:
                    continue
                partial += chunk.decode("utf-8", "replace").replace("\r", "")
                *done, partial = partial.split("\n")
                for d in done:
                    add(re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", d).replace("uart:~$ ", ""))
        except Exception as e:
            add(f"==== console error: {e}")
        finally:
            s.close()

cookie = None
def call(method, path, body=None, timeout=3):
    h = {"Host": HOST, "Origin": f"http://{HOST}"}
    if cookie:
        h["Cookie"] = cookie
    data = json.dumps(body).encode() if body is not None else None
    if data:
        h["Content-Type"] = "application/json"
    c = http.client.HTTPConnection(HOST, 80, timeout=timeout)
    c.request(method, "/api/v1" + path, body=data, headers=h)
    r = c.getresponse(); raw = r.read(); hdrs = {k.lower(): v for k, v in r.getheaders()}; c.close()
    return r.status, hdrs, (json.loads(raw) if raw else None)

def poll():
    global cookie
    while not stop.is_set():
        started = time.time()
        ping = subprocess.run(["ping", "-c", "1", "-t", "1", HOST], capture_output=True).returncode == 0
        summary = f"ping={'ok' if ping else 'lost'}"
        try:
            if cookie is None:
                s, h, b = call("POST", "/auth/session", {"password": PASSWORD}, timeout=10)
                if s == 200:
                    cookie = h["set-cookie"].split(";")[0]
            s, _, b = call("GET", "/network/status")
            if s == 401:
                cookie = None
            summary += f" http={s}"
            if s == 200:
                eth = next(i for i in b["interfaces"] if i["id"] == "ethernet")
                v4 = [a["address"] for a in eth["addresses"] if a["family"] == "ipv4"]
                summary += (f" eth={eth['state']} link={eth['link_up']} v4={','.join(v4) or '-'}"
                            f" default={b['default_interface']} dns={b['dns_servers']}")
        except Exception as e:
            summary += f" http=({type(e).__name__})"
        add("==== poll " + summary)
        time.sleep(max(0.0, EVERY - (time.time() - started)))

threading.Thread(target=console, daemon=True).start()
threading.Thread(target=poll, daemon=True).start()
add(f"==== watching board B for {SECONDS:.0f} s")
try:
    while time.time() - T0 < SECONDS:
        time.sleep(0.5)
        with lock:
            snapshot = list(lines)
        with open(OUT, "w") as f:
            f.write("\n".join(snapshot) + "\n")
finally:
    stop.set(); time.sleep(0.5)
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{len(lines)} lines -> {OUT}")
