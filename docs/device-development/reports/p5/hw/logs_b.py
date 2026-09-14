"""Board B only: the P5 scenarios - two log sources, wrap and gap, cursors across a
reboot, slow and stalled clients, the export, the UART's owner, C6 resets.

usage: logs_b.py <scenario> <out prefix> [args]
  smoke                 sources, a page, capabilities, coprocessor status: schema, both sources
  two-sources           C6 reset (ROM stream) + an STM32 debug burst through telnet, read live
  wrap                  a cursor left behind while the C6's ROM flood wraps its ring: gap, loss counters
  boot                  a cursor, a J-Link reset, the same cursor after boot: new boot_id, gap
  slow <minutes>        a page a minute; ping, HTTP, shell sampled meanwhile; lock hold times
  stalled               an export whose client stops reading: other clients' wait, then close
  export                full exports (ndjson, text): time, size, chunks, schema of every line
  uart                  console -> usb_bridge (shell) -> console, host not reading the CDC; flashing stand-in; DTR
  c6-reset <n>          n C6 resets through EN, 5 s apart: no fault, generation, markers
  stacks                thread stacks and heap from the console

Writes <prefix>.json (steps, host time) and <prefix>.console.log (same clock). Set
P5_IMAGE_SHA256 to have the image recorded in the JSON.
"""
import json
import socket
import sys
import threading
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b as b  # noqa: E402

SCENARIO, PREFIX, ARGS = sys.argv[1], sys.argv[2], sys.argv[3:]
rec = b.Record(PREFIX + ".json")
con = b.CONSOLE_READER
dev = b.Device()


def page(query=""):
    s, h, body, ms = dev.call("GET", "/logs/records" + query)
    errors = b.schema_errors("LogPage", body) if s == 200 else None
    return s, body, ms, errors


def status():
    return dev.call("GET", "/coprocessor/status")[2]


def esp32_since(cursor, seconds, step=1.0):
    """Follow source=esp32 from @p cursor for @p seconds; all records seen."""
    seen, t = [], time.time()
    while time.time() - t < seconds:
        s, body, _, _ = page(f"?source=esp32&limit=100&cursor={cursor}")
        if s == 200:
            seen.extend(body["items"])
            cursor = body["next_cursor"]
            if body["has_more"]:
                continue
        time.sleep(step)
    return seen, cursor


def markers_since(cursor, limit_pages=50):
    """Every coprocessor marker since @p cursor, paging to the end (the ROM flood
    would bury them in an unfiltered page)."""
    found, gap = [], False
    for _ in range(limit_pages):
        s, body, _, _ = page(f"?source=esp32&module=coprocessor&limit=100&cursor={cursor}")
        if s != 200:
            break
        gap = gap or body["gap"]
        found.extend((i["kind"], i["source_generation"], i["message"], i["seq"]) for i in body["items"])
        cursor = body["next_cursor"]
        if not body["has_more"]:
            break
    return found, gap


def tail_cursor(query="?limit=1"):
    s, body, _, _ = page(query)
    assert s == 200, body
    return body["next_cursor"]


def start():
    con.start()
    up = dev.wait_up(60)
    rec.step("device up", seconds=up)
    rec.step("login", ms=dev.login())
    st = dev.call("GET", "/system/status")[2]
    rec.step("system", boot_id=st["boot_id"], uptime_ms=st["uptime_ms"], firmware=st["firmware_version"])


def finish(**extra):
    con.shell("coproc logs", r"next seq", timeout=3)
    time.sleep(0.5)
    con.save(PREFIX + ".console.log")
    rec.save(**extra)
    con.stop()


# ---- scenarios -----------------------------------------------------------------------

def smoke():
    for name, path in (("LogSources", "/logs/sources"), ("Capabilities", "/capabilities"),
                       ("CoprocessorStatus", "/coprocessor/status")):
        s, _, body, ms = dev.call("GET", path)
        rec.step(path, status=s, ms=ms, schema=b.schema_errors(name, body), body=body)
    s, body, ms, errors = page("?limit=100")
    sources = sorted({i["source"] for i in body["items"]})
    seqs = [int(i["seq"]) for i in body["items"]]
    rec.step("page", status=s, ms=ms, schema=errors, items=len(body["items"]), sources=sources,
             ordered=seqs == sorted(seqs), has_more=body["has_more"], gap=body["gap"],
             bytes=len(json.dumps(body)), first=body["items"][:2], last=body["items"][-2:])


class Telnet:
    """Enough of a telnet client for Zephyr's shell (telnetlib is gone from Python 3.13):
    option negotiation from the server is read and ignored."""

    def __init__(self, host, port=23, timeout=5):
        b.refuse(host)
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.5)
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self):
        while True:
            try:
                if not self.sock.recv(4096):
                    return
            except socket.timeout:
                continue
            except OSError:
                return

    def write(self, data):
        self.sock.sendall(data)

    def close(self):
        self.sock.close()


def _missing_burst(items):
    seen = sorted(int(i["message"].split()[1]) for i in items if i["message"].startswith("burst "))
    return None if not seen else (seen[-1] - seen[0] + 1) - len(seen)


def two_sources():
    cursor = tail_cursor("?limit=1")
    # The burst is counted on its own filter: in source=all the C6's ROM flood
    # outruns a reader that takes about a second per page.
    cursor_stm32 = tail_cursor("?source=stm32&limit=1")
    t_burst = time.time()
    # The STM32's burst: debug output of a busy module, switched on over telnet so
    # the shell on the console stays free for the C6 reset.
    tn = Telnet(b.HOST, 23, timeout=5)
    time.sleep(0.5)
    tn.write(b"coproc burst 3000 120\r\n")
    rec.step("telnet: coproc burst 3000 120")
    line = con.shell("coproc reset", r"coproc reset: -?\d+", timeout=5)
    rec.step("C6 reset through EN", answer=line)
    # Traffic for net_tcp to talk about.
    for _ in range(20):
        dev.call("GET", "/system/status")
    items, t = [], time.time()
    while time.time() - t < 20:
        s, body, ms, errors = page(f"?limit=100&cursor={cursor}")
        if s == 200:
            items.extend(body["items"])
            cursor = body["next_cursor"]
            if errors:
                rec.step("schema errors", errors=errors[:5])
            if body["has_more"]:
                continue
        time.sleep(0.7)
    time.sleep(0.5)
    tn.close()
    seqs = [int(i["seq"]) for i in items]
    by_source = {s: sum(1 for i in items if i["source"] == s) for s in ("stm32", "esp32")}
    kinds = sorted({i["kind"] for i in items})
    resets = [i for i in items if i["kind"] == "reset"]
    rec.step("read live", records=len(items), by_source=by_source, kinds=kinds,
             ordered=seqs == sorted(seqs), unique=len(seqs) == len(set(seqs)),
             burst=sum(1 for i in items if i["message"].startswith("burst ")),
             burst_numbers_missing=_missing_burst(items),
             resets=resets[:3], banner=[i for i in items if "ESP-ROM" in i["message"]][:2],
             sample_stm32=[i for i in items if i["source"] == "stm32"][:3],
             sample_esp32=[i for i in items if i["source"] == "esp32"][:3])
    stm32, gap, pages = [], False, 0
    while pages < 80:
        s, body, ms, errors = page(f"?source=stm32&limit=100&cursor={cursor_stm32}")
        pages += 1
        if s != 200:
            break
        stm32.extend(body["items"])
        gap = gap or body["gap"]
        cursor_stm32 = body["next_cursor"]
        if not body["has_more"]:
            break
    burst = [i for i in stm32 if i["message"].startswith("burst ")]
    rec.step("the STM32 burst, source=stm32", pages=pages, records=len(stm32), burst=len(burst),
             burst_numbers_missing=_missing_burst(stm32), gap=gap,
             first=burst[0]["message"][:40] if burst else None, last=burst[-1]["message"][:40] if burst else None,
             seconds_since_burst=round(time.time() - t_burst, 1),
             shell_answer=con.shell("coproc logs", r"stm32: records", timeout=4))
    rec.step("sources", body=dev.call("GET", "/logs/sources")[2])


def _burst_stored(n):
    """Messages of this burst in the STM32 ring, by the unique "of <n> " in their text."""
    raw, _ = b.raw_get(f"/logs/export?source=stm32&contains=of+{n}+&max_records=2000", dev.cookie, timeout=90)
    head, body, sizes = b.dechunk(raw)
    numbers = []
    for line in body.decode("utf-8", "replace").splitlines():
        r = json.loads(line)
        parts = r["message"].split()
        if r["kind"] == "message" and len(parts) >= 4 and parts[0] == "burst" and parts[3] == str(n):
            numbers.append(int(parts[1]))
    return sorted(numbers)


def burst(n, length):
    """Every message of `coproc burst` is in the STM32 ring or counted in dropped_count.

    The command goes over telnet: the console drops input bytes, and a lost space once
    turned 1000 into 1000120 (logs/26). Counting is over HTTP, which a busy shell cannot
    delay."""
    def dropped():
        items = dev.call("GET", "/logs/sources")[2]["items"]
        return int(next(i for i in items if i["id"] == "stm32")["dropped_count"])

    d0 = dropped()
    tn = Telnet(b.HOST, 23, timeout=5)
    time.sleep(0.5)
    tn.write(f"coproc burst {n} {length}\r\n".encode())
    rec.step("burst sent over telnet", n=n, length=length, dropped_before=d0)
    last, stable = None, 0
    for _ in range(120):
        time.sleep(2)
        now_counts = (dropped(), len(_burst_stored(n)))
        stable = stable + 1 if now_counts == last else 0
        last = now_counts
        if stable >= 3 and now_counts[1] + now_counts[0] - d0 >= n:
            break
        if stable >= 6:
            break
    tn.close()
    numbers = _burst_stored(n)
    lost = last[0] - d0
    rec.step("burst accounted", n=n, length=length, stored=len(numbers), lost_delta=lost,
             stored_plus_lost=len(numbers) + lost, exact=len(numbers) + lost == n,
             stored_range=[numbers[0], numbers[-1]] if numbers else None,
             stored_contiguous=(numbers[-1] - numbers[0] + 1 == len(numbers)) if numbers else None,
             shell=con.shell("coproc logs", r"stm32: records", timeout=5))


def _thread_cpu():
    """Per-thread execution cycles from `kernel thread list` (THREAD_RUNTIME_STATS)."""
    import re
    t0 = b.now()
    con.shell("kernel thread list", r"idle", timeout=8)
    time.sleep(2)
    cycles, name = {}, None
    for line in con.since(t0):
        m = re.match(r"\s*\*?0x[0-9a-f]+\s+(\S+)?", line)
        if m and "options" not in line:
            name = m.group(1) or "?"
        m = re.search(r"Total execution cycles: (\d+)", line)
        if m and name:
            cycles[name] = int(m.group(1))
    return cycles


def bridge_load(seconds):
    """Network while the C6's stream flows through the USB bridge to a reading host."""
    import serial

    def probe(tag, duration):
        rows, t = [], time.time()
        while time.time() - t < duration:
            ts = b.now()
            ping = b.ping_once(timeout_ms=400)
            try:
                http = dev.call("GET", "/system/status", timeout=3)[3]
            except OSError as e:
                http = type(e).__name__
            rows.append({"t": ts, "ping_ms": ping, "http_ms": http})
            time.sleep(0.3)
        lost = sum(1 for r in rows if r["ping_ms"] is None)
        failed = sum(1 for r in rows if not isinstance(r["http_ms"], int))
        rec.step(f"probe: {tag}", samples=len(rows), ping_lost=lost, http_failed=failed, rows=rows)

    con.shell("coproc dtr off", r"dtr follow off", timeout=3)
    before = _thread_cpu()
    probe("console, nobody on the CDC", 15)
    rec.step("bridge on", answer=con.shell("coproc mode bridge", r"coproc mode bridge: -?\d+", timeout=5))
    port = b.cdc_port()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.05
    s.dtr = s.rts = False
    s.open()
    got = [0]
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            got[0] += len(s.read(4096))

    th = threading.Thread(target=reader, daemon=True)
    mid0 = _thread_cpu()
    th.start()
    probe("bridge, host reading the CDC", seconds)
    mid1 = _thread_cpu()
    stop.set()
    th.join(timeout=2)
    rec.step("host read", bytes=got[0])
    probe("bridge, host not reading", 15)
    s.close()
    rec.step("console again", answer=con.shell("coproc mode console", r"coproc mode console: -?\d+", timeout=5))
    after = _thread_cpu()
    delta = {k: mid1.get(k, 0) - mid0.get(k, 0) for k in mid1}
    total = sum(v for v in delta.values() if v > 0) or 1
    rec.step("thread cycles while the host read the bridge",
             share_percent={k: round(100 * v / total, 1) for k, v in sorted(delta.items(), key=lambda kv: -kv[1])[:10]})
    rec.step("coproc status", answer=con.shell("coproc status", r"bridge dropped to host \d+", timeout=4))
    con.shell("coproc dtr on", r"dtr follow on", timeout=3)


def dtr(rounds):
    """Opening the CDC (DTR high) hands the UART to the bridge; closing it gives it back.
    Every shell answer is checked: a lost byte on the console once left DTR following
    off and the bridge never came (logs/31)."""
    import serial
    answer = con.shell("coproc dtr on", r"dtr follow on", timeout=3, retries=5)
    rec.step("dtr follow", answer=answer)
    if answer is None:
        raise RuntimeError("the shell did not confirm `coproc dtr on`")
    port = b.cdc_port()
    results = []
    for i in range(rounds):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.2
        s.dtr = True
        t = time.monotonic()
        s.open()
        to_bridge = None
        while time.monotonic() - t < 6:
            if status()["uart_mode"] == "usb_bridge":
                to_bridge = round(time.monotonic() - t, 2)
                break
            time.sleep(0.1)
        got = len(s.read(4096))
        s.close()
        t = time.monotonic()
        to_console = None
        while time.monotonic() - t < 8:
            if status()["uart_mode"] == "console":
                to_console = round(time.monotonic() - t, 2)
                break
            time.sleep(0.1)
        results.append({"round": i, "to_bridge_s": to_bridge, "bytes_read": got, "to_console_s": to_console})
        time.sleep(1)
    rec.step("DTR rounds", results=results,
             coproc=con.shell("coproc status", r"switches \d+", timeout=4))


def wrap():
    cursor = tail_cursor("?source=esp32&limit=1")
    line = con.shell("coproc logs", r"esp32: records", timeout=3)
    rec.step("before", esp32=line, status=status())
    # Keep the C6 printing: its ROM loops on "invalid header" after a reset.
    con.shell("coproc reset", r"coproc reset", timeout=5)
    t = time.time()
    while time.time() - t < 120:
        line = con.shell("coproc logs", r"esp32: records", timeout=3)
        rec.step("esp32 ring", line=line)
        if line and "overwritten 0" not in line:
            break
        time.sleep(10)
    time.sleep(30)
    s, body, ms, errors = page(f"?source=esp32&limit=100&cursor={cursor}")
    rec.step("old cursor after the wrap", status=s, gap=body.get("gap"), items=len(body.get("items", [])),
             first_seq=body["items"][0]["seq"] if body.get("items") else None, schema=errors,
             dropped=body.get("dropped_count"))
    s, body, ms, errors = page("?source=esp32&limit=5&cursor=" + cursor[:-4] + "AAAA")
    rec.step("damaged cursor", status=s, body=body)
    s, body, ms, errors = page("?source=stm32&limit=5&cursor=" + cursor)
    rec.step("cursor of other filters", status=s, body=body)
    rec.step("sources", body=dev.call("GET", "/logs/sources")[2])


def boot():
    s, body, _, _ = page("?limit=5")
    cursor, old_boot = body["next_cursor"], body["boot_id"]
    while int(dev.call("GET", "/system/status")[2]["uptime_ms"]) < 31000:
        time.sleep(2)   # a shorter boot counts towards the network's five-boot recovery
    r = b.jlink("r\ng\n")
    rec.step("J-Link reset", rc=r.returncode)
    time.sleep(5)
    rec.step("up again", seconds=dev.wait_up(120))
    rec.step("login", ms=dev.login())
    s, body, ms, errors = page(f"?limit=5&cursor={cursor}")
    rec.step("cursor of the previous boot", status=s, gap=body.get("gap"), boot_id=body.get("boot_id"),
             old_boot_id=old_boot, items=[(i["seq"], i["source"], i["message"][:60]) for i in body["items"]],
             schema=errors)
    raw, seconds = b.raw_get("/logs/export?source=stm32&max_records=2000", dev.cookie, timeout=60)
    head, body_bytes, sizes = b.dechunk(raw)
    records = [json.loads(l) for l in body_bytes.decode("utf-8", "replace").splitlines()]
    rec.step("earliest STM32 records of this boot", count=len(records),
             first=[(r["seq"], r["uptime_ms"], r["level"], r["module"], r["message"][:70]) for r in records[:12]],
             before_main=[r["message"][:70] for r in records if int(r["uptime_ms"]) < 2033][:12])


def slow(minutes):
    cursor = tail_cursor()
    stop = threading.Event()
    samples = []

    def sampler():
        while not stop.is_set():
            t = time.time()
            ping = b.ping_once()
            try:
                s, _, _, ms = dev.call("GET", "/system/status", timeout=10)
            except OSError:
                s, ms = None, None
            shell = con.shell("kernel uptime", r"Uptime: \d+", timeout=3, retries=1)
            samples.append({"t": b.now(), "ping_ms": ping, "http": s, "http_ms": ms, "shell": shell is not None})
            time.sleep(max(0.0, 5 - (time.time() - t)))

    th = threading.Thread(target=sampler, daemon=True)
    th.start()
    for minute in range(minutes):
        time.sleep(60)
        s, body, ms, errors = page(f"?limit=100&cursor={cursor}")
        rec.step("a page a minute", minute=minute + 1, status=s, ms=ms, gap=body.get("gap"),
                 items=len(body.get("items", [])), has_more=body.get("has_more"),
                 dropped=body.get("dropped_count"))
        cursor = body.get("next_cursor", cursor)
    stop.set()
    th.join(timeout=10)
    http_ms = sorted(x["http_ms"] for x in samples if x["http_ms"] is not None)
    rec.step("samples", count=len(samples),
             ping_lost=sum(1 for x in samples if x["ping_ms"] is None),
             http_failed=sum(1 for x in samples if x["http"] != 200),
             shell_failed=sum(1 for x in samples if not x["shell"]),
             http_p50=http_ms[len(http_ms) // 2] if http_ms else None,
             http_max=http_ms[-1] if http_ms else None, all=samples)


def stalled():
    waits = []
    # A 4 KiB receive buffer: the window closes almost at once, as for a frozen tab
    # on a slow link. (Run 1 without it: macOS took the whole export into its own
    # buffer, so the server never had to wait and simply finished.)
    sock = b.raw_get("/logs/export", dev.cookie, read=False, rcvbuf=4096)
    sock.recv(512)
    rec.step("export started, client stops reading", rcvbuf=sock.getsockopt(b.socket.SOL_SOCKET, b.socket.SO_RCVBUF))
    t_stall = time.time()
    for _ in range(6):
        t = time.monotonic()
        try:
            s, _, _, _ = dev.call("GET", "/system/status", timeout=30)
        except OSError as e:
            s = str(e)
        waits.append({"t": round(time.time() - t_stall, 2), "status": s,
                      "wait_ms": round((time.monotonic() - t) * 1000)})
        time.sleep(1)
    rec.step("other client during the stall", waits=waits)
    # Does the server still hold the stalled connection?
    sock.settimeout(2)
    try:
        rest = sock.recv(65536)
        rec.step("stalled client reads again", bytes=len(rest), closed=len(rest) == 0)
    except (socket.timeout, OSError) as e:
        rec.step("stalled client reads again", error=str(e))
    sock.close()
    rec.step("shell during", answer=con.shell("kernel uptime", r"Uptime", timeout=3))


def export():
    for fmt in ("ndjson", "text"):
        concurrent = []
        stop = threading.Event()

        def poll():
            while not stop.is_set():
                t = time.monotonic()
                try:
                    s = dev.call("GET", "/system/status", timeout=30)[0]
                except OSError:
                    s = None
                concurrent.append(round((time.monotonic() - t) * 1000))
                time.sleep(0.2)

        th = threading.Thread(target=poll, daemon=True)
        th.start()
        raw, seconds = b.raw_get(f"/logs/export?format={fmt}&max_records=2000", dev.cookie, timeout=60)
        stop.set()
        th.join(timeout=35)
        head, body, sizes = b.dechunk(raw)
        lines = body.decode("utf-8", "replace").splitlines()
        errors, kinds = [], {}
        if fmt == "ndjson":
            for line in lines:
                record = json.loads(line)
                kinds[record["kind"]] = kinds.get(record["kind"], 0) + 1
                errors.extend(b.schema_errors("LogRecord", record)[:1])
        rec.step(f"export {fmt}", seconds=seconds, bytes=len(body), lines=len(lines), chunks=len(sizes),
                 chunk_max=max(sizes) if sizes else 0, kinds=kinds, schema_errors=errors[:5],
                 head=head.splitlines()[:8], first=lines[:2], concurrent_status_ms=concurrent)


def uart():
    import serial
    rec.step("status before", body=status())
    cursor = tail_cursor("?source=esp32&module=coprocessor&limit=1")
    line = con.shell("coproc dtr off", r"dtr follow off", timeout=3)
    line = con.shell("coproc mode bridge", r"coproc mode bridge: -?\d+ in \d+ ms", timeout=5)
    rec.step("shell: console -> usb_bridge", answer=line, status=status(),
             sources=dev.call("GET", "/logs/sources")[2])
    port = b.cdc_port()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False
    s.open()
    data = bytearray()
    t = time.time()
    while time.time() - t < 5:
        data.extend(s.read(4096))
    rec.step("host reads the CDC for 5 s", port=port, bytes=len(data), sample=bytes(data[:160]).decode("utf-8", "replace"))
    # The host stops reading: the board must not care.
    samples = []
    for _ in range(12):
        t_sample = b.now()
        try:
            http = dev.call("GET", "/system/status", timeout=15)[0]
        except OSError as e:
            http = f"{type(e).__name__}"
        samples.append({"t": t_sample, "ping_ms": b.ping_once(), "http": http,
                        "shell": con.shell("kernel uptime", r"Uptime", timeout=3, retries=1) is not None})
        time.sleep(5)
    rec.step("host not reading the CDC for 60 s", samples=samples,
             coproc=con.shell("coproc status", r"bridge dropped to host \d+", timeout=3))
    s.close()
    line = con.shell("coproc mode console", r"coproc mode console: -?\d+ in \d+ ms", timeout=5)
    rec.step("shell: usb_bridge -> console", answer=line, status=status())
    time.sleep(2)
    line = con.shell("coproc mode flashing", r"coproc mode flashing: -?\d+ in \d+ ms", timeout=5)
    rec.step("shell: console -> flashing (stand-in)", answer=line, status=status(),
             capabilities=dev.call("GET", "/capabilities")[2]["features"]["esp32_logs"])
    time.sleep(3)
    line = con.shell("coproc mode console", r"coproc mode console: -?\d+ in \d+ ms", timeout=5)
    rec.step("shell: flashing -> console", answer=line, status=status())
    time.sleep(2)
    markers, gap = markers_since(cursor)
    rec.step("markers", markers=markers, gap=gap)
    # DTR: opening the port hands the UART to the bridge, closing gives it back.
    con.shell("coproc dtr on", r"dtr follow on", timeout=3)
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = True
    t = time.monotonic()
    s.open()
    mode, t_bridge = None, None
    while time.monotonic() - t < 5:
        mode = status()["uart_mode"]
        if mode == "usb_bridge":
            t_bridge = round(time.monotonic() - t, 2)
            break
        time.sleep(0.1)
    rec.step("DTR high", mode=mode, seconds=t_bridge)
    s.close()
    t = time.monotonic()
    t_console = None
    while time.monotonic() - t < 8:
        mode = status()["uart_mode"]
        if mode == "console":
            t_console = round(time.monotonic() - t, 2)
            break
        time.sleep(0.1)
    rec.step("DTR low", mode=mode, seconds=t_console)


def c6_reset(n):
    cursor = tail_cursor("?source=esp32&module=coprocessor&limit=1")
    g0 = status()["generation"]
    for i in range(n):
        rec.step("reset", i=i, answer=con.shell("coproc reset", r"coproc reset: -?\d+", timeout=5))
        time.sleep(5)
    time.sleep(10)
    markers, gap = markers_since(cursor)
    faults = [l for l in con.since(0) if any(k in l for k in ("FAULT", "Halting", "Faulting", "ASSERTION"))]
    rec.step("after", generation_before=g0, generation_after=status()["generation"],
             markers_through_en=sum(1 for m in markers if m[2].startswith("ESP32 reset through EN")),
             markers_by_itself=sum(1 for m in markers if "by itself" in m[2]),
             markers_gap=gap, first_markers=markers[:8],
             faults=faults, ping_ms=b.ping_once(), http=dev.call("GET", "/system/status")[0])


def stacks():
    con.shell("kernel thread stacks", r"(unused|Unused)", timeout=5)
    time.sleep(3)
    con.shell("kernel heap", r"heap", timeout=3, retries=1)
    time.sleep(1)
    rec.step("stacks", lines=[l for l in con.since(0) if "unused" in l or "0x" in l][-60:])


SCENARIOS = {
    "smoke": smoke, "two-sources": two_sources, "wrap": wrap, "boot": boot,
    "slow": lambda: slow(int(ARGS[0]) if ARGS else 5), "stalled": stalled, "export": export,
    "uart": uart,
    "dtr": lambda: dtr(int(ARGS[0]) if ARGS else 3),
    "bridge-load": lambda: bridge_load(int(ARGS[0]) if ARGS else 20),
    "burst": lambda: burst(int(ARGS[0]) if ARGS else 1000, int(ARGS[1]) if len(ARGS) > 1 else 120), "c6-reset": lambda: c6_reset(int(ARGS[0]) if ARGS else 5), "stacks": stacks,
}

if __name__ == "__main__":
    if SCENARIO not in SCENARIOS:
        sys.exit(__doc__)
    start()
    try:
        SCENARIOS[SCENARIO]()
    except Exception as e:  # noqa: BLE001 - recorded, then re-raised
        rec.step("scenario failed", error=repr(e))
        finish()
        raise
    finish()
