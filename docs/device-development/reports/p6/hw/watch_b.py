"""Board B only: watch a coprocessor install that a person runs from the web UI.

Nothing is sent to the C6 and no install is started here: the serial console is recorded
with the host clock, and every 2 s the API is read - CoprocessorStatus, the active
coprocessor_update job (phase, progress, error), last_update, and new W5500 receive-state
events from the STM32 log. Stops after <minutes> or on Ctrl-C and writes everything.

usage: watch_b.py <out prefix> <minutes>
"""
import json
import sys
import time
import urllib.parse

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b as b  # noqa: E402

PREFIX, MINUTES = sys.argv[1], float(sys.argv[2])
rec = b.Record(PREFIX + ".json")
con = b.CONSOLE_READER
dev = b.Device()
W5500 = "/logs/records?source=stm32&limit=100&contains=" + urllib.parse.quote("receive state inconsistent")


def safe(method, path):
    """One read; any failure (socket error, a chunked body the board cut short -
    http.client.IncompleteRead, which is not an OSError) is recorded, never fatal."""
    try:
        return dev.call(method, path, timeout=10)
    except Exception as e:  # noqa: BLE001 - the watcher must outlive a bad answer
        rec.step("request failed", path=path, error=repr(e)[:200])
        return None, None, repr(e), None


def main():
    con.start()
    rec.step("http up", seconds=dev.wait_up(120))
    rec.step("login", ms=dev.login())
    last_status, last_job, seen_w5500 = None, None, set()
    end = time.time() + MINUTES * 60
    while time.time() < end:
        s, _, st, ms = safe("GET", "/coprocessor/status")
        if s == 401:
            dev.login()
            continue
        if s == 200:
            view = {k: st.get(k) for k in ("state", "uart_mode", "transport_ready", "firmware_version",
                                           "generation", "last_update")}
            if view != last_status:
                rec.step("coprocessor/status", ms=ms, **view)
                last_status = view
        s, _, sysst, _ = safe("GET", "/system/status")
        for jid in (sysst or {}).get("active_job_ids", []) if s == 200 else []:
            js, _, job, jms = safe("GET", f"/jobs/{jid}")
            if js == 200 and job.get("kind") == "coprocessor_update":
                view = {k: job.get(k) for k in ("id", "state", "phase", "progress", "cancellable", "error")}
                if view != last_job:
                    rec.step("install job", ms=jms, **view)
                    last_job = view
        s, _, logs, _ = safe("GET", W5500)
        if s == 200:
            new = [i for i in logs.get("items", []) if i["seq"] not in seen_w5500]
            if new:
                seen_w5500.update(i["seq"] for i in new)
                rec.step("w5500 receive-state events", new=len(new), total=len(seen_w5500),
                         last_uptime_ms=new[-1]["uptime_ms"])
        time.sleep(2)


try:
    main()
except KeyboardInterrupt:
    rec.step("stopped by Ctrl-C")
finally:
    rec.save()
    con.save(PREFIX + ".console.log")
    con.stop()
    print(json.dumps({"saved": PREFIX}))
