# SPDX-License-Identifier: Apache-2.0
"""Board A only (192.168.88.14, ST-Link 002F002B3233510739363634): the STM32 update through
API v1, as the web page does it, with the console recorded alongside.

usage:
  web_update_a.py <workdir> status                     capabilities, /system/status, /system/firmware
  web_update_a.py <workdir> upload <signed.bin>        create (target stm32u585), chunks, verify
  web_update_a.py <workdir> install <upload_id> [ack]  startSystemUpdate, job phases, wait for the
                                                       new boot_id, sign in again, /system/firmware
  web_update_a.py <workdir> full <signed.bin> [ack]    upload + install
  web_update_a.py <workdir> delete <upload_id>

Signs in with WEB_PASSWORD (default: the bench password of board A); when setup is still open
it first sets that password through the setup token the device publishes. Every step is one
JSON line in <workdir>/web.jsonl; the console goes to <workdir>/console.log.
"""
import hashlib
import http.client
import json
import os
import secrets
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../../../../../tests/bench"))
import bench_console  # noqa: E402

HOST = "192.168.88.14"
REFUSED = ("192.168.88.13", "000941000024", "3543501200210047")
PASSWORD = os.environ.get("WEB_PASSWORD", "cedar-bench-P2-changed")
CHUNK = 16384

WORK, CMD, ARGS = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(WORK, exist_ok=True)
assert HOST not in REFUSED
session = {"cookie": None, "csrf": None}


def log(step, **fields):
    line = {"t": time.strftime("%H:%M:%S"), "step": step, **fields}
    with open(os.path.join(WORK, "web.jsonl"), "a") as f:
        f.write(json.dumps(line, ensure_ascii=False) + "\n")
    print(json.dumps(line, ensure_ascii=False), flush=True)


def request(method, path, body=None, content_type=None, key=None, timeout=30):
    headers = {"Host": HOST, "Origin": f"http://{HOST}"}
    if session["cookie"]:
        headers["Cookie"] = session["cookie"]
    if session["csrf"] and method != "GET":
        headers["X-CSRF-Token"] = session["csrf"]
    if key:
        headers["Idempotency-Key"] = key
    data = None
    if isinstance(body, dict):
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    elif body is not None:
        data = body
        headers["Content-Type"] = content_type
    c = http.client.HTTPConnection(HOST, 80, timeout=timeout)
    t = time.monotonic()
    c.request(method, "/api/v1" + path, body=data, headers=headers)
    r = c.getresponse()
    raw = r.read()
    ms = round((time.monotonic() - t) * 1000)
    set_cookie = r.getheader("Set-Cookie")
    c.close()
    try:
        parsed = json.loads(raw) if raw else None
    except ValueError:
        parsed = raw[:200].decode(errors="replace")
    return r.status, parsed, ms, set_cookie


def new_key():
    return "stmupd-" + secrets.token_hex(10)


def sign_in():
    s, state, _, _ = request("GET", "/auth/state")
    if s == 200 and state.get("setup_required"):
        c = http.client.HTTPConnection(HOST, 80, timeout=60)
        c.request("POST", "/api/v1/auth/setup", body=json.dumps({"password": PASSWORD}).encode(),
                  headers={"Host": HOST, "Origin": f"http://{HOST}", "Content-Type": "application/json",
                           "X-Setup-Token": state["setup_token"]})
        r = c.getresponse()
        log("setup", status=r.status, body=r.read()[:200].decode(errors="replace"))
        c.close()
    s, body, ms, cookie = request("POST", "/auth/session", {"password": PASSWORD}, timeout=60)
    if s not in (200, 201) or not cookie:
        log("sign in failed", status=s, body=body)
        sys.exit(1)
    session["cookie"] = cookie.split(";")[0]
    session["csrf"] = body["csrf_token"]
    log("signed in", ms=ms)


def wait_job(job_id, limit=600, until_gone=False):
    t = time.monotonic()
    last_phase = None
    while time.monotonic() - t < limit:
        try:
            s, job, _, _ = request("GET", f"/jobs/{job_id}", timeout=10)
        except OSError as err:
            if until_gone:
                return {"lost": str(err), "last_phase": last_phase}, round(time.monotonic() - t, 1)
            time.sleep(0.5)
            continue
        if s != 200:
            return {"status": s, "body": job, "last_phase": last_phase}, round(time.monotonic() - t, 1)
        if job["phase"] != last_phase:
            last_phase = job["phase"]
            log("job phase", job=job_id, phase=last_phase, state=job["state"],
                at_s=round(time.monotonic() - t, 1))
        if job["state"] in ("succeeded", "failed", "cancelled", "interrupted"):
            return job, round(time.monotonic() - t, 1)
        time.sleep(0.2)
    return None, limit


def status():
    for path in ("/capabilities", "/system/status", "/system/firmware"):
        s, body, ms, _ = request("GET", path)
        if path == "/capabilities" and s == 200:
            body = {"stm32_update": body["features"]["stm32_update"],
                    "system_upload_max_bytes": body["limits"]["system_upload_max_bytes"],
                    "firmware_formats": body["firmware_formats"]}
        log("GET " + path, status=s, ms=ms, body=body)


def upload(path):
    data = open(path, "rb").read()
    body = {"filename": os.path.basename(path), "size_bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(), "target": "stm32u585"}
    s, up, ms, _ = request("POST", "/firmware/uploads", body, key=new_key())
    log("create", status=s, ms=ms, body=up, file=path, size=len(data), sha256=body["sha256"])
    if s != 201:
        return None
    t0 = time.monotonic()
    puts, jobs = [], []
    offset = 0
    while offset < len(data):
        piece = data[offset:offset + CHUNK]
        s, acc, put_ms, _ = request("PUT", f"/firmware/uploads/{up['id']}/data?offset={offset}", piece,
                                    "application/octet-stream", key=new_key())
        if s != 202:
            log("chunk refused", offset=offset, status=s, body=acc)
            return None
        job, job_s = wait_job(acc["job_id"], limit=120)
        puts.append(put_ms)
        jobs.append(job_s)
        if not job or job.get("state") != "succeeded":
            log("chunk failed", offset=offset, job=job)
            return None
        offset += len(piece)
    seconds = round(time.monotonic() - t0, 1)
    jobs_sorted = sorted(jobs)
    log("chunks done", bytes=offset, seconds=seconds, kib_s=round(offset / 1024 / seconds, 1),
        put_ms_p50=sorted(puts)[len(puts) // 2], put_ms_max=max(puts),
        job_s_p50=jobs_sorted[len(jobs) // 2], job_s_max=jobs_sorted[-1])
    s, acc, _, _ = request("POST", f"/firmware/uploads/{up['id']}/verify", {}, key=new_key())
    job, job_s = wait_job(acc["job_id"], limit=300) if s == 202 else (acc, 0)
    s, got, _, _ = request("GET", f"/firmware/uploads/{up['id']}")
    log("verify", seconds=job_s, job_state=job and job.get("state"), upload=got)
    return got


def install(upload_id, ack):
    s, before, _, _ = request("GET", "/system/status")
    boot_before = before["boot_id"]
    s, acc, ms, _ = request("POST", "/system/updates",
                            {"upload_id": upload_id, "acknowledge_downgrade": ack}, key=new_key())
    log("startSystemUpdate", status=s, ms=ms, body=acc)
    if s != 202:
        return
    t0 = time.monotonic()
    job, job_s = wait_job(acc["job_id"], limit=60, until_gone=True)
    log("job end seen", after_s=job_s, job=job)
    # The device restarts, MCUboot swaps (~36 s), the application starts.
    first_answer = None
    while time.monotonic() - t0 < 300:
        try:
            s, st, _, _ = request("GET", "/system/status", timeout=5)
        except OSError:
            time.sleep(1)
            continue
        if first_answer is None:
            first_answer = round(time.monotonic() - t0, 1)
        if s == 401:
            session["cookie"] = session["csrf"] = None
            try:
                sign_in()
            except (OSError, SystemExit):
                time.sleep(2)
            continue
        if s == 200 and st["boot_id"] != boot_before:
            log("back", after_s=round(time.monotonic() - t0, 1), first_answer_s=first_answer,
                firmware_version=st["firmware_version"], boot_id=st["boot_id"])
            break
        time.sleep(1)
    else:
        log("device did not come back", seconds=300)
        return
    status()


console = bench_console.Console(os.path.join(WORK, "console.log"))
try:
    sign_in()
    if CMD == "status":
        status()
    elif CMD == "upload":
        upload(ARGS[0])
    elif CMD == "install":
        install(ARGS[0], len(ARGS) > 1 and ARGS[1] == "ack")
    elif CMD == "full":
        up = upload(ARGS[0])
        if up and up.get("state") == "ready":
            install(up["id"], len(ARGS) > 1 and ARGS[1] == "ack")
    elif CMD == "delete":
        s, acc, _, _ = request("DELETE", f"/firmware/uploads/{ARGS[0]}", key=new_key())
        log("delete", status=s, body=acc)
    else:
        sys.exit(f"unknown command {CMD}")
finally:
    time.sleep(1)
    console.close()
