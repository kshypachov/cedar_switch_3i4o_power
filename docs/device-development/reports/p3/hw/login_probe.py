"""Time web sign-ins every <interval> s for <seconds> s; print one line each.

usage: login_probe.py <out> <seconds> [interval]
"""
import http.client, json, sys, time
out, seconds = sys.argv[1], float(sys.argv[2])
interval = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
end = time.monotonic() + seconds
with open(out, "w") as f:
    while time.monotonic() < end:
        t0 = time.monotonic(); stamp = time.strftime("%H:%M:%S")
        try:
            c = http.client.HTTPConnection("192.168.88.14", 80, timeout=10)
            c.request("POST", "/api/v1/auth/session", body=json.dumps({"password": "cedar-bench-P2-changed"}),
                      headers={"Host": "192.168.88.14", "Origin": "http://192.168.88.14", "Content-Type": "application/json"})
            r = c.getresponse(); r.read(); c.close()
            line = f"{stamp} login {r.status} {time.monotonic() - t0:.2f}s"
        except Exception as e:  # the browser's own timeout is 10 s
            line = f"{stamp} login FAILED {type(e).__name__} after {time.monotonic() - t0:.2f}s"
        print(line, flush=True); f.write(line + "\n"); f.flush()
        time.sleep(max(0.0, interval - (time.monotonic() - t0)))
