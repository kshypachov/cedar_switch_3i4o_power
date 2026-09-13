"""Probe the board every <interval> s for <seconds> s: ping, telnet shell, web sign-in.

usage: probes.py <out> <seconds> [interval]
One line per round, so a stall shows which of the three stopped answering:
ping (network stack and W5500), shell (a thread of its own), sign-in (HTTP
server and the derivation thread).
"""
import http.client, json, re, socket, subprocess, sys, threading, time

HOST = "192.168.88.14"
out, seconds = sys.argv[1], float(sys.argv[2])
interval = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0

def ping():
    t0 = time.monotonic()
    r = subprocess.run(["ping", "-c", "1", "-W", "2000", HOST], capture_output=True, text=True)
    return f"{(time.monotonic() - t0) * 1000:.0f}ms" if r.returncode == 0 else "lost"

class Shell:
    def __init__(self):
        self.s = None
    def probe(self):
        t0 = time.monotonic()
        try:
            if self.s is None:
                self.s = socket.create_connection((HOST, 23), timeout=5)
                self.s.settimeout(0.2)
                time.sleep(0.5)
                try:
                    self.s.recv(65536)
                except socket.timeout:
                    pass
            self.s.sendall(b"kernel uptime\r\n")
            buf = b""; end = time.monotonic() + 5
            while time.monotonic() < end:
                try:
                    buf += self.s.recv(4096)
                except socket.timeout:
                    pass
                if b"Uptime:" in buf:
                    return f"{(time.monotonic() - t0) * 1000:.0f}ms"
            return "noanswer"
        except OSError as e:
            self.s = None
            return f"err:{type(e).__name__}"

def login():
    t0 = time.monotonic()
    try:
        c = http.client.HTTPConnection(HOST, 80, timeout=10)
        c.request("POST", "/api/v1/auth/session", body=json.dumps({"password": "cedar-bench-P2-changed"}),
                  headers={"Host": HOST, "Origin": f"http://{HOST}", "Content-Type": "application/json"})
        r = c.getresponse(); r.read(); c.close()
        return f"{r.status}/{time.monotonic() - t0:.2f}s"
    except Exception as e:
        return f"FAILED:{type(e).__name__}/{time.monotonic() - t0:.2f}s"

shell = Shell()
end = time.monotonic() + seconds
with open(out, "w") as f:
    while time.monotonic() < end:
        t0 = time.monotonic(); stamp = time.strftime("%H:%M:%S")
        results = {}
        threads = [threading.Thread(target=lambda k=k, fn=fn: results.__setitem__(k, fn())) for k, fn in
                   (("ping", ping), ("shell", shell.probe), ("login", login))]
        for t in threads: t.start()
        for t in threads: t.join()
        line = f"{stamp} ping {results['ping']:>8} shell {results['shell']:>10} login {results['login']}"
        print(line, flush=True); f.write(line + "\n"); f.flush()
        time.sleep(max(0.0, interval - (time.monotonic() - t0)))
