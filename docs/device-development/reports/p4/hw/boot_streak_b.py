"""Board B only: the five-short-boots recovery, driven by J-Link resets instead of the
owner's power cycles. The count does not look at the reset cause, so a debugger reset
counts like a power cycle; what a reset cannot show is a power loss during the write
of the count itself.

Each short boot: reset, wait for the console's "boot N of 5 in a row shorter than 30 s"
(the count is stored by then), reset again before 30 s. The pattern is a string:
  S = short boot (reset right after the count line)
  L = long boot (stays up 40 s, which must clear the count)
The final boot is left running, and the network configuration and status are read.

usage: boot_streak_b.py <out.log> <out.json> <pattern>     e.g. SSSSS, or SSLS
"""
import http.client, json, os, re, subprocess, sys, tempfile, threading, time

HOST = "192.168.88.13"
PASSWORD = "cedar-bench-P4-boardB"
UART = "/dev/cu.usbmodem5AE60208891"
JLINK = ["JLinkExe", "-USB", "000941000024", "-device", "STM32U585AI", "-if", "SWD",
         "-speed", "4000", "-autoconnect", "1", "-NoGui", "1", "-ExitOnError", "1"]
OUT_LOG, OUT_JSON, PATTERN = sys.argv[1], sys.argv[2], sys.argv[3]
T0 = time.time()
lines, lock = [], threading.Lock()
result = {"pattern": PATTERN, "when": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "boots": []}

def add(text):
    with lock:
        lines.append(f"[+{time.time() - T0:8.2f}] {text}")

def reader(stop):
    import serial
    s = serial.Serial(); s.port, s.baudrate, s.timeout = UART, 115200, 0.1
    s.dtr = s.rts = False; s.open()
    partial = ""
    while not stop.is_set():
        chunk = s.read(4096)
        if not chunk:
            continue
        partial += chunk.decode("utf-8", "replace").replace("\r", "")
        *done, partial = partial.split("\n")
        for d in done:
            add(re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", d).replace("uart:~$ ", ""))
    s.close()

def reset():
    with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as f:
        f.write("r\ng\nq\n"); script = f.name
    r = subprocess.run(JLINK + ["-CommanderScript", script], capture_output=True, text=True)
    os.unlink(script)
    add(f"==== host: J-Link reset rc={r.returncode}")
    if r.returncode != 0:
        raise SystemExit("J-Link reset failed")
    return time.time()

def wait_for(pattern, since_index, limit):
    t = time.time()
    while time.time() - t < limit:
        with lock:
            for line in lines[since_index:]:
                m = re.search(pattern, line)
                if m:
                    return m, line
        time.sleep(0.1)
    return None, None

def call(method, path, body=None, cookie=None):
    h = {"Host": HOST, "Origin": f"http://{HOST}"}
    if cookie:
        h["Cookie"] = cookie
    data = json.dumps(body).encode() if body is not None else None
    if data:
        h["Content-Type"] = "application/json"
    c = http.client.HTTPConnection(HOST, 80, timeout=15)
    c.request(method, "/api/v1" + path, body=data, headers=h)
    r = c.getresponse(); raw = r.read(); hdrs = {k.lower(): v for k, v in r.getheaders()}; c.close()
    return r.status, hdrs, (json.loads(raw) if raw else None)

stop = threading.Event()
threading.Thread(target=reader, args=(stop,), daemon=True).start()
time.sleep(0.3)
try:
    for n, kind in enumerate(PATTERN, 1):
        with lock:
            mark = len(lines)
        at = reset()
        m, line = wait_for(r"boot (\d+) of (\d+) in a row shorter than", mark, 60)
        boot = {"n": n, "kind": kind, "count_line": line}
        restore, rline = wait_for(r"short boots in a row: restoring", mark, 0.5)
        if m:
            boot["count"] = int(m.group(1))
            boot["count_line_after_reset_s"] = round(time.time() - at, 1)
        if kind == "S":
            time.sleep(1.0)            # the count is stored before the line is logged
        else:
            time.sleep(40.0)
            _, cleared = wait_for(r"clearing the boot count failed", mark, 0.1)
            boot["clear_failed_line"] = cleared
        restore, rline = wait_for(r"short boots in a row: restoring", mark, 0.1)
        boot["restore_line"] = rline
        boot["up_s"] = round(time.time() - at, 1)
        result["boots"].append(boot)
        add(f"==== host: boot {n} ({kind}) up {boot['up_s']} s, count {boot.get('count')}")
    # The last boot stays up: read what it came up with.
    time.sleep(45)
    s, h, b = call("POST", "/auth/session", {"password": PASSWORD})
    cookie = h["set-cookie"].split(";")[0] if s == 200 else None
    result["login"] = s
    for path in ("/network/config", "/network/status"):
        s, _, b = call("GET", path, cookie=cookie)
        result[path] = {"status": s, "body": b}
finally:
    stop.set(); time.sleep(0.3)
    with open(OUT_LOG, "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(OUT_JSON, "w") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    print(json.dumps(result, ensure_ascii=False)[:3000])
