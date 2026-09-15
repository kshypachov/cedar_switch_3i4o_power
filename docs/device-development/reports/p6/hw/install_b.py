"""Board B only: start the coprocessor install of a staged, verified upload and follow it.

The owner authorised starting the install from the bench (2026-09-14, after two attempts
from the web UI hung in entering_bootloader): the file was uploaded and verified through
the web UI, this script only sends startCoprocessorUpdate for it, over Ethernet, with
acknowledge_recovery=true - exactly what the ESP32 screen sends.

usage: install_b.py <upload id> <out prefix> [minutes]

Every 2 s: the job (phase, progress, cancellable, error), CoprocessorStatus (state,
uart_mode, firmware_version, last_update) and new W5500 receive-state events. The
serial console is recorded with the host clock. When the board stops answering the
script records it and keeps trying until the end; it never resets the board.
"""
import os
import secrets
import sys
import time
import urllib.parse

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b as b  # noqa: E402

UPLOAD, PREFIX = sys.argv[1], sys.argv[2]
MINUTES = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
# P6_QUIET=1 (after review of attempt 7): the job only, every 15 s, no status and no W5500
# log query - three new TCP connections every 2 s went through the W5500 during the session.
QUIET = os.environ.get("P6_QUIET") == "1"
POLL_S = 15.0 if QUIET else 2.0
rec = b.Record(PREFIX + ".json")
con = b.CONSOLE_READER
dev = b.Device()
W5500 = "/logs/records?source=stm32&limit=100&contains=" + urllib.parse.quote("receive state inconsistent")


def safe(method, path, body=None, key=None):
    try:
        if key is None:
            return dev.call(method, path, body, timeout=15)
        # board_b.Device.call has no Idempotency-Key argument; a POST with one.
        import http.client
        import json
        headers = {"Host": b.HOST, "Origin": f"http://{b.HOST}", "Cookie": dev.cookie,
                   "X-CSRF-Token": dev.csrf, "Idempotency-Key": key, "Content-Type": "application/json"}
        c = http.client.HTTPConnection(b.HOST, 80, timeout=15)
        t = time.monotonic()
        c.request(method, "/api/v1" + path, body=json.dumps(body).encode(), headers=headers)
        r = c.getresponse()
        raw = r.read()
        c.close()
        return r.status, None, json.loads(raw) if raw else None, round((time.monotonic() - t) * 1000)
    except Exception as e:  # noqa: BLE001 - a board that stops answering is data, not a crash
        return None, None, repr(e)[:200], None


def main():
    b.refuse(b.HOST)
    con.start()
    rec.step("http up", seconds=dev.wait_up(120))
    rec.step("login", ms=dev.login())
    s, _, up, ms = safe("GET", f"/firmware/uploads/{UPLOAD}")
    rec.step("upload", status=s, ms=ms, state=(up or {}).get("state") if s == 200 else up,
             image=(up or {}).get("image") if s == 200 else None)
    if s != 200 or up.get("state") != "ready":
        return
    # Matter's init and the settings file backend write /lfs for the first minutes after a
    # boot; an install started 55 s after boot timed out in preflight (hw/logs/10).
    while True:
        s, _, sysst, _ = safe("GET", "/system/status")
        uptime = int(sysst["uptime_ms"]) if s == 200 else 0
        if uptime >= 120000:
            break
        time.sleep(5)
    rec.step("board settled", uptime_ms=uptime)
    s, _, st, _ = safe("GET", "/coprocessor/status")
    rec.step("status before", status=s, body=st)
    body = {"upload_id": UPLOAD, "method": "uart", "acknowledge_recovery": True}
    s, _, acc, ms = safe("POST", "/coprocessor/updates", body, key="p6-bench-" + secrets.token_hex(8))
    rec.step("startCoprocessorUpdate", status=s, ms=ms, body=acc)
    if s != 202:
        return
    job_id, last_view, last_status, seen, silent_since = acc["job_id"], None, None, set(), None
    t0 = time.monotonic()
    end = time.time() + MINUTES * 60
    while time.time() < end:
        s, _, job, ms = safe("GET", f"/jobs/{job_id}")
        if s == 401:
            dev.login()
            continue
        if s == 200:
            silent_since = None
            view = {k: job.get(k) for k in ("state", "phase", "progress", "cancellable", "error")}
            if view != last_view:
                rec.step("job", elapsed_s=round(time.monotonic() - t0, 1), ms=ms, **view)
                last_view = view
        else:
            if silent_since is None:
                silent_since = time.monotonic()
                rec.step("board not answering", status=s, error=job)
        if QUIET and s == 200 and job.get("state") not in ("succeeded", "failed", "cancelled",
                                                            "interrupted"):
            time.sleep(POLL_S)
            continue
        s2, _, st, _ = safe("GET", "/coprocessor/status")
        if s2 == 200:
            sview = {k: st.get(k) for k in ("state", "uart_mode", "transport_ready", "firmware_version",
                                            "host_protocol", "last_update")}
            if sview != last_status:
                rec.step("status", elapsed_s=round(time.monotonic() - t0, 1), **sview)
                last_status = sview
        s3, _, logs, _ = safe("GET", W5500)
        if s3 == 200:
            new = [i for i in logs.get("items", []) if i["seq"] not in seen]
            if new:
                seen.update(i["seq"] for i in new)
                rec.step("w5500 events", new=len(new), total=len(seen))
        if s == 200 and job.get("state") in ("succeeded", "failed", "cancelled", "interrupted"):
            rec.step("finished", elapsed_s=round(time.monotonic() - t0, 1), state=job["state"])
            time.sleep(20)  # the boot log of the new firmware, into the console record
            break
        time.sleep(POLL_S)


try:
    main()
except KeyboardInterrupt:
    rec.step("stopped by Ctrl-C")
finally:
    rec.save()
    con.save(PREFIX + ".console.log")
    con.stop()
