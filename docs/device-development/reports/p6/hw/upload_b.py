"""Board B only: P6 step 1 - upload and verify of coprocessor images, and the refusals.
Nothing is ever written to the C6: startCoprocessorUpdate is refused by this script
before any request (owner's decision - the C6 of board B is written only by the
acceptance run through the web UI).

usage: upload_b.py <scenario> <out prefix> [args]
  upload <file>             create, chunks (time per PUT and per job), verify, Upload JSON; deleted after
  stall <file>              the same with GET /system/status every 0.5 s in parallel: latency while chunks commit
  corpus <dir>              every fixture of tests/fixtures/esp32/firmware: verify outcome vs manifest.json
  protocol <file>           offset_mismatch, replay, key conflict, no Content-Type, 413 chunk, second upload,
                            too large, logout in the middle and resume
  reset <file> <chunks>     J-Link reset after <chunks> chunks: received_bytes after boot, resume, verify

Writes <prefix>.json and <prefix>.console.log. P6_IMAGE_SHA256 is recorded when set.
"""
import hashlib
import http.client
import json
import os
import secrets
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b as b  # noqa: E402

SCENARIO, PREFIX, ARGS = sys.argv[1], sys.argv[2], sys.argv[3:]
rec = b.Record(PREFIX + ".json")
con = b.CONSOLE_READER
dev = b.Device()
CHUNK = 16384
FORBIDDEN = "/coprocessor/updates"


def request(method, path, body=None, content_type=None, key=None, timeout=30):
    """Any method with a raw or JSON body, the session's cookie and CSRF token."""
    if FORBIDDEN in path:
        sys.exit("this script never starts a coprocessor update")
    b.refuse(b.HOST)
    headers = {"Host": b.HOST, "Origin": f"http://{b.HOST}"}
    if dev.cookie:
        headers["Cookie"] = dev.cookie
    if dev.csrf and method != "GET":
        headers["X-CSRF-Token"] = dev.csrf
    if key:
        headers["Idempotency-Key"] = key
    data = None
    if isinstance(body, (dict, list)):
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    elif body is not None:
        data = body
        if content_type:
            headers["Content-Type"] = content_type
    c = http.client.HTTPConnection(b.HOST, 80, timeout=timeout)
    t = time.monotonic()
    c.request(method, "/api/v1" + path, body=data, headers=headers)
    r = c.getresponse()
    raw = r.read()
    ms = round((time.monotonic() - t) * 1000)
    c.close()
    try:
        parsed = json.loads(raw) if raw else None
    except ValueError:
        parsed = raw[:200]
    return r.status, parsed, ms


def new_key():
    return "p6-" + secrets.token_hex(10)


def wait_job(job_id, limit=120):
    t = time.monotonic()
    while time.monotonic() - t < limit:
        s, job, _ = request("GET", f"/jobs/{job_id}")
        if s == 200 and job["state"] in ("succeeded", "failed", "cancelled", "interrupted"):
            return job, round((time.monotonic() - t) * 1000)
        time.sleep(0.1)
    return None, None


def create(data, name):
    body = {"filename": name, "size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    s, up, ms = request("POST", "/firmware/uploads", body, key=new_key())
    rec.step("create", file=name, size=len(data), status=s, ms=ms,
             schema=b.schema_errors("Upload", up) if s == 201 else None, body=up if s != 201 else None)
    return up if s == 201 else None


def send_chunks(up, data, start=0, stop=None, record_each=True):
    offset = start
    times = []
    while offset < len(data) and (stop is None or offset < stop):
        piece = data[offset:offset + CHUNK]
        s, acc, put_ms = request("PUT", f"/firmware/uploads/{up['id']}/data?offset={offset}", piece,
                                 "application/octet-stream", key=new_key())
        if s != 202:
            rec.step("chunk refused", offset=offset, status=s, body=acc)
            return offset, times
        job, job_ms = wait_job(acc["job_id"])
        times.append((put_ms, job_ms))
        if record_each:
            rec.step("chunk", offset=offset, put_ms=put_ms, job_ms=job_ms, state=job and job["state"],
                     error=job and job["error"])
        if not job or job["state"] != "succeeded":
            return offset, times
        offset += len(piece)
    return offset, times


def verify(up):
    s, acc, ms = request("POST", f"/firmware/uploads/{up['id']}/verify", {}, key=new_key())
    if s != 202:
        rec.step("verify refused", status=s, body=acc)
        return None
    job, job_ms = wait_job(acc["job_id"], limit=300)
    s, got, _ = request("GET", f"/firmware/uploads/{up['id']}")
    rec.step("verify", job_ms=job_ms, job_state=job and job["state"], job_error=job and job["error"],
             upload_state=got and got.get("state"), image=got and got.get("image"),
             error=got and got.get("error"), schema=b.schema_errors("Upload", got) if s == 200 else None)
    return got


def delete(up):
    s, acc, _ = request("DELETE", f"/firmware/uploads/{up['id']}", key=new_key())
    job = wait_job(acc["job_id"])[0] if s == 202 else None
    rec.step("delete", status=s, job_state=job and job["state"])


def summary(times):
    puts = sorted(p for p, _ in times)
    jobs = sorted(j for _, j in times if j is not None)
    pick = lambda a, q: a[min(len(a) - 1, int(len(a) * q))] if a else None  # noqa: E731
    return {"chunks": len(times), "put_ms_p50": pick(puts, 0.5), "put_ms_max": puts[-1] if puts else None,
            "job_ms_p50": pick(jobs, 0.5), "job_ms_p95": pick(jobs, 0.95), "job_ms_max": jobs[-1] if jobs else None}


def upload(path, parallel_status=False):
    data = Path(path).read_bytes()
    up = create(data, Path(path).name)
    if not up:
        return
    samples, stop = [], threading.Event()

    def poll():
        while not stop.is_set():
            try:
                _, _, ms = request("GET", "/system/status", timeout=15)
            except OSError:
                ms = None
            samples.append(ms)
            time.sleep(0.5)

    if parallel_status:
        threading.Thread(target=poll, daemon=True).start()
    t = time.monotonic()
    offset, times = send_chunks(up, data, record_each=False)
    stop.set()
    rec.step("chunks done", bytes=offset, of=len(data), seconds=round(time.monotonic() - t, 1),
             **summary(times))
    if parallel_status:
        ok = sorted(x for x in samples if x is not None)
        rec.step("status while uploading", samples=len(samples), failed=sum(x is None for x in samples),
                 p50=ok[len(ok) // 2] if ok else None, p95=ok[int(len(ok) * 0.95)] if ok else None,
                 max=ok[-1] if ok else None)
    if offset == len(data):
        verify(up)
    delete(up)


def corpus(folder):
    manifest = json.loads((Path(folder) / "manifest.json").read_text())
    for name, meta in manifest.items():
        data = (Path(folder) / name).read_bytes()
        up = create(data, name)
        if not up:
            continue
        offset, _ = send_chunks(up, data, record_each=False)
        got = verify(up) if offset == len(data) else None
        code = (got or {}).get("error") and got["error"]["code"]
        rec.step("corpus result", file=name, expected=meta["expected"], got_state=(got or {}).get("state"),
                 got_code=code, match=(meta["expected"] is None and (got or {}).get("state") == "ready")
                 or code == meta["expected"])
        delete(up)


def protocol(path):
    data = Path(path).read_bytes()
    up = create(data, "protocol.bin")
    base = f"/firmware/uploads/{up['id']}/data"
    s, body, _ = request("PUT", f"{base}?offset={CHUNK}", data[:CHUNK], "application/octet-stream", key=new_key())
    rec.step("wrong offset", status=s, code=body and body.get("error", {}).get("code"))
    s, body, _ = request("PUT", f"{base}?offset=0", data[:CHUNK], None, key=new_key())
    rec.step("no content type", status=s, code=body and body.get("error", {}).get("code"))
    s, body, _ = request("PUT", f"{base}?offset=0", data[:CHUNK + 1], "application/octet-stream", key=new_key())
    rec.step("chunk over 16384", status=s, code=body and body.get("error", {}).get("code"))
    key = new_key()
    s1, a1, _ = request("PUT", f"{base}?offset=0", data[:CHUNK], "application/octet-stream", key=key)
    s2, a2, _ = request("PUT", f"{base}?offset=0", data[:CHUNK], "application/octet-stream", key=key)
    rec.step("replay same key", first=s1, second=s2, same_job=bool(a1 and a2 and a1.get("job_id") == a2.get("job_id")))
    wait_job(a1["job_id"])
    s3, a3, _ = request("PUT", f"{base}?offset=0", data[CHUNK:2 * CHUNK], "application/octet-stream", key=key)
    rec.step("same key, other bytes", status=s3, code=a3 and a3.get("error", {}).get("code"))
    s, body, _ = request("POST", "/firmware/uploads",
                         {"filename": "second.bin", "size_bytes": 10, "sha256": "0" * 64}, key=new_key())
    rec.step("second upload", status=s, code=body and body.get("error", {}).get("code"))
    delete(up)
    s, body, _ = request("POST", "/firmware/uploads",
                         {"filename": "big.bin", "size_bytes": 0x1d0001, "sha256": "0" * 64}, key=new_key())
    rec.step("too large", status=s, code=body and body.get("error", {}).get("code"))
    up = create(data, "logout.bin")
    offset, _ = send_chunks(up, data, stop=4 * CHUNK, record_each=False)
    s, _, _ = request("DELETE", "/auth/session")
    dev.cookie = dev.csrf = None
    s2, body, _ = request("GET", f"/firmware/uploads/{up['id']}")
    rec.step("after logout", logout=s, get=s2, code=body and body.get("error", {}).get("code"))
    rec.step("login again", ms=dev.login())
    s, got, _ = request("GET", f"/firmware/uploads/{up['id']}")
    rec.step("resume point", status=s, received=got and got["received_bytes"], sent=offset)
    offset, _ = send_chunks(up, data, start=got["received_bytes"], record_each=False)
    verify(up) if offset == len(data) else None
    delete(up)


def reset_mid(path, chunks):
    data = Path(path).read_bytes()
    up = create(data, "reset.bin")
    offset, _ = send_chunks(up, data, stop=chunks * CHUNK, record_each=False)
    rec.step("before reset", sent=offset)
    r = b.jlink("r\ng\n")
    rec.step("J-Link reset", rc=r.returncode)
    dev.cookie = dev.csrf = None
    rec.step("http up", seconds=dev.wait_up(120))
    rec.step("login", ms=dev.login())
    s, got, _ = request("GET", f"/firmware/uploads/{up['id']}")
    rec.step("after boot", status=s, state=got and got.get("state"), received=got and got.get("received_bytes"))
    if s != 200:
        return
    offset, times = send_chunks(up, data, start=got["received_bytes"], record_each=False)
    rec.step("resumed", bytes=offset, **summary(times))
    if offset == len(data):
        verify(up)
    delete(up)


def main():
    con.start()
    up = dev.wait_up(60)
    rec.step("http up", seconds=up)
    if up is None:
        return
    rec.step("login", ms=dev.login())
    if SCENARIO == "upload":
        upload(ARGS[0])
    elif SCENARIO == "stall":
        upload(ARGS[0], parallel_status=True)
    elif SCENARIO == "corpus":
        corpus(ARGS[0])
    elif SCENARIO == "protocol":
        protocol(ARGS[0])
    elif SCENARIO == "reset":
        reset_mid(ARGS[0], int(ARGS[1]))
    else:
        sys.exit(f"unknown scenario {SCENARIO}")


try:
    main()
finally:
    rec.save()
    con.save(PREFIX + ".console.log")
    con.stop()
