"""Board B only: the P4 network API scenarios, with timings and schema checks.

Board B: DHCP 192.168.88.13, MAC 80:34:28:10:6a:1d, console /dev/cu.usbmodem5AE60208891,
J-Link 000941000024. Board A belongs to another session. The script sends only to board
B's DHCP address or to a static address in 192.168.88.200-250 (outside everything the
bench uses), and a static address is applied only after a ping and the ARP entry it
leaves show nothing answers there.

usage: net_api_b.py <out.json> <scenario> [args] [--host IP] [--console LOG]
  reads                   status, config, capabilities x5: timings, schema errors
  invalid                 candidates the device must refuse (no network change)
  static <ip>             DHCP -> static <ip>/24 gw .1: apply, reach the new address, confirm there
  dhcp                    back to Ethernet DHCP from --host, confirm on the DHCP address
  timeout <ip>            static <ip>, apply with 60 s and never confirm: rollback by deadline
  rollback <ip>           static <ip>, apply, then DELETE from the new address
  dns                     manual DNS 9.9.9.9 + 1.1.1.1, confirm; status dns_servers
  dns-auto                automatic DNS back, confirm
  scan                    Wi-Fi scan (board B: C6 has no firmware, expect 503)
  wifi                    stage Wi-Fi enabled (expect 422 not_allowed while the C6 is absent)
  idempotency             stage twice with one key, then the key with another body
  reboot-staged           stage, reset through J-Link, read the transaction after boot
  reboot-awaiting <ip>    static <ip>, apply, reset while awaiting; read it back on DHCP
Every response body with a 2xx schema is validated against openapi.json.
"""
import http.client, json, os, re, subprocess, sys, tempfile, threading, time, uuid
from pathlib import Path

BOARD_A = "192.168.88.14"   # refused outright, before the allowlist below
DHCP_HOST = "192.168.88.13"
STATIC_RANGE = ("192.168.88.", 200, 250)
BOARD_B_MAC = "80:34:28:10:6a:1d"
GATEWAY = "192.168.88.1"
PASSWORD = "cedar-bench-P4-boardB"
UART = "/dev/cu.usbmodem5AE60208891"
JLINK = ["JLinkExe", "-USB", "000941000024", "-device", "STM32U585AI", "-if", "SWD",
         "-speed", "4000", "-autoconnect", "1", "-NoGui", "1", "-ExitOnError", "1"]
REPO = Path(__file__).resolve().parents[5]
DOC = json.loads((REPO / "docs/device-development/openapi.json").read_text())

args = sys.argv[1:]
def option(name):
    if name in args:
        i = args.index(name); value = args[i + 1]; del args[i:i + 2]; return value
    return None
HOST = option("--host") or DHCP_HOST
CONSOLE = option("--console")
OUT, SCENARIO, REST = Path(args[0]), args[1], args[2:]

def refuse_board_a(ip):
    """Board B's DHCP address, or a static one in the range; anything else is refused."""
    prefix, low, high = STATIC_RANGE
    ip = ip.strip()
    if ip == BOARD_A:
        sys.exit(f"{ip} is board A's address; refusing")
    if ip == DHCP_HOST:
        return
    if ip.startswith(prefix) and ip[len(prefix):].isdigit() and low <= int(ip[len(prefix):]) <= high:
        return
    sys.exit(f"{ip}: not board B's DHCP address and not in {prefix}{low}-{high}; refusing")
refuse_board_a(HOST)
for a in REST:
    if re.fullmatch(r"[0-9.]+", a):
        refuse_board_a(a)

from jsonschema import Draft202012Validator
from referencing import Registry, Resource
from referencing.jsonschema import DRAFT202012
REGISTRY = Registry().with_resource("doc", Resource.from_contents({"components": DOC["components"]}, default_specification=DRAFT202012))
def schema_errors(name, body):
    # With formats asserted: otherwise "9.9.9.9" matches both oneOf branches (ipv4, ipv6).
    v = Draft202012Validator({"$ref": f"doc#/components/schemas/{name}"}, registry=REGISTRY,
                             format_checker=Draft202012Validator.FORMAT_CHECKER)
    return [f"{list(e.absolute_path)}: {e.message}" for e in v.iter_errors(body)]

T0 = time.time()
def now():
    return round(time.time() - T0, 2)

# ---- console, host-timestamped, so the device log and these steps share a clock
console_lines = []
stop_console = threading.Event()
def console_reader():
    import serial
    s = serial.Serial(); s.port, s.baudrate, s.timeout = UART, 115200, 0.1
    s.dtr = s.rts = False; s.open()
    partial = ""
    while not stop_console.is_set():
        chunk = s.read(4096)
        if not chunk:
            continue
        partial += chunk.decode("utf-8", "replace").replace("\r", "")
        *done, partial = partial.split("\n")
        console_lines.extend(f"[+{now():8.2f}] {line}" for line in done)
    s.close()
if CONSOLE:
    threading.Thread(target=console_reader, daemon=True).start()
    time.sleep(0.3)

steps = []
def step(what, **data):
    entry = {"t": now(), "step": what, **data}
    steps.append(entry)
    print(json.dumps(entry, ensure_ascii=False)[:400], flush=True)
    if CONSOLE:
        console_lines.append(f"[+{now():8.2f}] ==== host: {what}")

# ---- HTTP
class Device:
    def __init__(self, host):
        refuse_board_a(host)
        self.host, self.cookie, self.csrf = host, None, None

    def call(self, method, path, body=None, headers=None, timeout=15):
        h = {"Host": self.host, "Origin": f"http://{self.host}"}
        if self.cookie:
            h["Cookie"] = self.cookie
        if self.csrf and method != "GET":
            h["X-CSRF-Token"] = self.csrf
        h.update(headers or {})
        data = None
        if body is not None:
            data = json.dumps(body).encode(); h["Content-Type"] = "application/json"
        c = http.client.HTTPConnection(self.host, 80, timeout=timeout)
        t = time.monotonic()
        c.request(method, "/api/v1" + path, body=data, headers=h)
        r = c.getresponse(); raw = r.read(); dt = time.monotonic() - t
        hdrs = {k.lower(): v for k, v in r.getheaders()}
        c.close()
        try:
            parsed = json.loads(raw) if raw else None
        except ValueError:
            parsed = raw.decode(errors="replace")
        return r.status, hdrs, parsed, round(dt * 1000)

    def login(self):
        s, h, b, ms = self.call("POST", "/auth/session", {"password": PASSWORD})
        if s != 200:
            raise RuntimeError(f"login on {self.host}: {s} {b}")
        self.cookie = h["set-cookie"].split(";")[0]
        self.csrf = b["csrf_token"]
        return ms

    def wait_up(self, limit):
        t = time.monotonic()
        while time.monotonic() - t < limit:
            try:
                s, _, _, _ = self.call("GET", "/auth/state", timeout=2)
                if s == 200:
                    return round(time.monotonic() - t, 2)
            except OSError:
                pass
            time.sleep(0.5)
        return None

def key():
    return "p4-" + uuid.uuid4().hex[:20]

def expect_schema(name, status, body, ok=(200, 201, 202)):
    return schema_errors(name, body) if status in ok else None

def address_is_free(ip):
    """A ping and the ARP entry it leaves: something that drops ping still answers ARP."""
    refuse_board_a(ip)
    ping = subprocess.run(["ping", "-c", "3", "-t", "4", ip], capture_output=True, text=True)
    arp = subprocess.run(["arp", "-n", ip], capture_output=True, text=True).stdout
    mac = re.search(r"at ((?:[0-9a-f]{1,2}:){5}[0-9a-f]{1,2})", arp)
    mac = ":".join(f"{int(x, 16):02x}" for x in mac.group(1).split(":")) if mac else None
    # An entry holding board B's own MAC is the cache remembering board B's previous
    # use of this address; with no ping reply nothing else holds it now.
    answered = ping.returncode == 0 or (mac is not None and mac != BOARD_B_MAC)
    step("free-address check", ip=ip, ping_rc=ping.returncode, arp=arp.strip(), arp_mac=mac)
    return not answered

def jlink_reset(device):
    """Reset only a board up for more than 30 s: a shorter boot counts towards the
    five-boot recovery, and a run of reboot scenarios would restore the factory
    network configuration under the test."""
    while True:
        s, _, b, _ = device.call("GET", "/system/status")
        if s != 200:
            sys.exit(f"system status before reset: {s} {b}")
        uptime = int(b["uptime_ms"])   # an int64 is a string in the contract
        if uptime >= 31000:
            break
        time.sleep((31000 - uptime) / 1000 + 0.5)
    step("uptime before reset", uptime_ms=b["uptime_ms"])
    with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as f:
        f.write("r\ng\nq\n"); script = f.name
    r = subprocess.run(JLINK + ["-CommanderScript", script], capture_output=True, text=True)
    os.unlink(script)
    step("J-Link reset", rc=r.returncode)
    if r.returncode != 0:
        sys.exit("J-Link reset failed:\n" + r.stdout[-2000:])

def poll_job(dev, job_id, limit=120):
    t = time.monotonic()
    while True:
        s, _, b, _ = dev.call("GET", f"/jobs/{job_id}")
        if s != 200 or b["state"] in ("succeeded", "failed", "cancelled", "interrupted") \
                or time.monotonic() - t > limit:
            return s, b, round(time.monotonic() - t, 2)
        time.sleep(0.3)

def current(dev):
    s, _, cfg, ms = dev.call("GET", "/network/config")
    assert s == 200, (s, cfg)
    return cfg

def ipv4(mode, address=None, prefix=None, gateway=None):
    return {"mode": mode, "address": address, "prefix_length": prefix, "gateway": gateway}

def candidate(cfg, ethernet_ipv4=None, dns=None, wifi_enabled=None):
    c = json.loads(json.dumps(cfg["config"]))
    wifi = c["interfaces"]["wifi"]
    wifi.pop("password_set", None)
    # Disabled Wi-Fi is not judged; the stored password stays as it is.
    wifi["credential"] = {"action": "keep"}
    if ethernet_ipv4 is not None:
        c["interfaces"]["ethernet"]["ipv4"] = ethernet_ipv4
    if dns is not None:
        c["dns"] = dns
    if wifi_enabled is not None:
        wifi["enabled"] = wifi_enabled
    return {"base_revision": cfg["revision"], "config": c}

def stage(dev, body, k=None):
    s, h, b, ms = dev.call("POST", "/network/transactions", body, {"Idempotency-Key": k or key()})
    step("stage", status=s, ms=ms, body=b, schema_errors=expect_schema("NetworkTransaction", s, b, (201,)))
    return s, b

def apply(dev, txn, timeout=120):
    s, h, b, ms = dev.call("POST", f"/network/transactions/{txn}/apply",
                           {"confirmation_timeout_seconds": timeout}, {"Idempotency-Key": key()})
    step("apply", status=s, ms=ms, body=b, schema_errors=expect_schema("JobAccepted", s, b, (202,)))
    return s, b

def transaction(dev, txn):
    s, _, b, ms = dev.call("GET", f"/network/transactions/{txn}")
    step("transaction", host=dev.host, status=s, ms=ms, body=b,
         schema_errors=expect_schema("NetworkTransaction", s, b, (200,)))
    return s, b

def confirm(dev, txn):
    s, _, b, ms = dev.call("POST", f"/network/transactions/{txn}/confirm", {}, {"Idempotency-Key": key()})
    step("confirm", host=dev.host, status=s, ms=ms, body=b)
    if s == 202:
        js, job, waited = poll_job(dev, b["job_id"])
        step("confirm job", host=dev.host, job=job, waited_s=waited)
    return s, b

def wait_state(dev, txn, states, limit):
    t = time.monotonic()
    while time.monotonic() - t < limit:
        try:
            s, _, b, _ = dev.call("GET", f"/network/transactions/{txn}", timeout=3)
            if s != 200 or b["state"] in states:
                return s, b, round(time.monotonic() - t, 2)
        except OSError:
            pass
        time.sleep(0.5)
    return None, None, round(time.monotonic() - t, 2)

def to_static(dev, ip, timeout=120):
    if not address_is_free(ip):
        sys.exit(f"{ip} answered: not free, nothing staged")
    cfg = current(dev)
    s, txn = stage(dev, candidate(cfg, ipv4("static", ip, 24, GATEWAY)))
    if s != 201:
        sys.exit("stage refused")
    t_apply = time.monotonic()
    s, job = apply(dev, txn["id"], timeout)
    if s != 202:
        sys.exit("apply refused")
    new = Device(ip)
    up = new.wait_up(60)
    step("new address answers", ip=ip, after_apply_s=round(time.monotonic() - t_apply, 2) if up is not None else None)
    old_answers = Device(dev.host).wait_up(2)
    step("old address after apply", ip=dev.host, answers=old_answers is not None)
    return txn, job, new

dev = Device(HOST)
out = {"scenario": SCENARIO, "host": HOST, "when": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "steps": steps}
try:
    step("login", host=HOST, ms=dev.login())

    if SCENARIO == "reads":
        for path, schema in (("/network/status", "NetworkStatus"), ("/network/config", "NetworkConfigResponse"),
                             ("/capabilities", "Capabilities")):
            times, last = [], None
            for _ in range(5):
                s, _, b, ms = dev.call("GET", path); times.append(ms); last = (s, b)
            step("read", path=path, status=last[0], p50_ms=sorted(times)[2], max_ms=max(times),
                 schema_errors=expect_schema(schema, last[0], last[1]), body=last[1])

    elif SCENARIO == "invalid":
        cfg = current(dev)
        cases = {
            "prefix 31": ipv4("static", "192.168.88.213", 31, GATEWAY),
            "gateway off subnet": ipv4("static", "192.168.88.213", 24, "192.168.89.1"),
            "gateway is the network address": ipv4("static", "192.168.88.213", 24, "192.168.88.0"),
            "address is the broadcast": ipv4("static", "192.168.88.255", 24, GATEWAY),
            "address loopback": ipv4("static", "127.0.0.2", 8, None),
            "static without prefix": ipv4("static", "192.168.88.213", None, GATEWAY),
            "dhcp with an address": ipv4("dhcp", "192.168.88.213", 24, None),
        }
        for name, v4 in cases.items():
            s, b = stage(dev, candidate(cfg, v4))
            steps[-1]["case"] = name
        for name, dns in {"dns unspecified": {"mode": "manual", "servers": ["0.0.0.0"]},
                          "dns three servers": {"mode": "manual", "servers": ["9.9.9.9", "1.1.1.1", "8.8.8.8"]},
                          "dns manual empty": {"mode": "manual", "servers": []},
                          "dns not an address": {"mode": "manual", "servers": ["dns.example"]}}.items():
            s, b = stage(dev, candidate(cfg, dns=dns))
            steps[-1]["case"] = name
        stale = candidate(cfg); stale["base_revision"] = cfg["revision"] + 7
        stage(dev, stale); steps[-1]["case"] = "stale base_revision"

    elif SCENARIO == "static":
        txn, job, new = to_static(dev, REST[0])
        step("login on the new address", ms=new.login())
        transaction(new, txn["id"])
        confirm(new, txn["id"])
        transaction(new, txn["id"])
        step("config after", body=current(new))
        s, _, b, _ = new.call("GET", "/network/status"); step("status after", body=b)

    elif SCENARIO == "dhcp":
        cfg = current(dev)
        s, txn = stage(dev, candidate(cfg, ipv4("dhcp")))
        t_apply = time.monotonic()
        apply(dev, txn["id"])
        back = Device(DHCP_HOST)
        up = back.wait_up(90)
        step("DHCP address answers", ip=DHCP_HOST, after_apply_s=round(time.monotonic() - t_apply, 2) if up is not None else None)
        step("login on the DHCP address", ms=back.login())
        transaction(back, txn["id"])
        confirm(back, txn["id"])
        s, _, b, _ = back.call("GET", "/network/status"); step("status after", body=b)

    elif SCENARIO == "timeout":
        txn, job, new = to_static(dev, REST[0], timeout=60)
        if new.wait_up(5) is not None:
            new.login()
            transaction(new, txn["id"])
        # The session lives on the device, not on the address the client used:
        # the cookie from the old address is tried on the DHCP one, and whether
        # it is accepted is recorded rather than assumed.
        back = Device(DHCP_HOST)
        back.cookie, back.csrf = dev.cookie, dev.csrf
        s, b, waited = wait_state(back, txn["id"], ("rolled_back", "failed"), 150)
        if s == 401:
            step("old session refused on the DHCP address; signing in again")
            if back.wait_up(60) is not None:
                back.login()
                s, b, waited = wait_state(back, txn["id"], ("rolled_back", "failed"), 150)
        step("rolled back by the deadline, read on the DHCP address", status=s, waited_s=waited, body=b)
        if back.wait_up(5) is not None:
            back.login()
            if b and b.get("job_id"):
                js, jb, _ = poll_job(back, b["job_id"])
                step("job", body=jb)
            transaction(back, txn["id"])

    elif SCENARIO == "rollback":
        txn, job, new = to_static(dev, REST[0])
        step("login on the new address", ms=new.login())
        s, _, b, ms = new.call("DELETE", f"/network/transactions/{txn['id']}", None, {"Idempotency-Key": key()})
        step("rollback request", status=s, ms=ms, body=b)
        back = Device(DHCP_HOST)
        up = back.wait_up(90)
        step("DHCP address answers after rollback", after_s=up)
        back.login()
        transaction(back, txn["id"])
        if s == 202:
            js, jb, waited = poll_job(back, b["job_id"]); step("rollback job", body=jb)

    elif SCENARIO in ("dns", "dns-auto"):
        cfg = current(dev)
        dns = {"mode": "manual", "servers": ["9.9.9.9", "1.1.1.1"]} if SCENARIO == "dns" else {"mode": "automatic", "servers": []}
        s, txn = stage(dev, candidate(cfg, dns=dns))
        apply(dev, txn["id"])
        s, b, waited = wait_state(dev, txn["id"], ("awaiting_confirmation", "failed", "rolled_back"), 30)
        step("awaiting", waited_s=waited, body=b)
        confirm(dev, txn["id"])
        s, _, b, _ = dev.call("GET", "/network/status"); step("status after", dns_servers=b.get("dns_servers"), body=b)

    elif SCENARIO == "scan":
        s, _, b, ms = dev.call("POST", "/network/wifi/scans", {}, {"Idempotency-Key": key()})
        step("scan", status=s, ms=ms, body=b)
        if s == 202:
            js, jb, waited = poll_job(dev, b["job_id"], 30); step("scan job", body=jb, waited_s=waited)
            s2, _, b2, ms2 = dev.call("GET", f"/network/wifi/scans/{b['job_id']}")
            step("scan results", status=s2, ms=ms2, body=b2, schema_errors=expect_schema("ScanResults", s2, b2, (200,)))

    elif SCENARIO == "wifi":
        cfg = current(dev)
        body = candidate(cfg, wifi_enabled=True)
        w = body["config"]["interfaces"]["wifi"]
        w.update({"ssid_base64": "Q2VkYXJUZXN0", "security": "wpa2_psk", "credential": {"action": "replace", "value": "not-a-real-password"}})
        stage(dev, body)
        s, _, b, _ = dev.call("GET", "/network/status"); step("status", body=b)

    elif SCENARIO == "idempotency":
        cfg = current(dev)
        k = key()
        body = candidate(cfg, dns={"mode": "manual", "servers": ["9.9.9.9"]})
        s1, b1 = stage(dev, body, k)
        s2, b2 = stage(dev, body, k)
        other = candidate(cfg, dns={"mode": "manual", "servers": ["1.1.1.1"]})
        s3, b3 = stage(dev, other, k)
        s4, b4 = stage(dev, other)
        step("summary", same_key_same_id=(b1 or {}).get("id") == (b2 or {}).get("id"), other_body=s3, second_transaction=s4)
        if s1 == 201:
            s, _, b, ms = dev.call("DELETE", f"/network/transactions/{b1['id']}", None, {"Idempotency-Key": key()})
            step("discard", status=s, body=b)

    elif SCENARIO == "apply-only":
        # The owner cuts power while this transaction awaits confirmation: the
        # longest deadline the contract allows, so the cut lands before it.
        txn, job, new = to_static(dev, REST[0], timeout=300)
        if new.wait_up(5) is not None:
            new.login()
            transaction(new, txn["id"])
        step("awaiting: the owner may cut power now", transaction=txn["id"])

    elif SCENARIO == "read-txn":
        transaction(dev, REST[0])
        step("config", body=current(dev))
        s, _, b, _ = dev.call("GET", "/network/status"); step("status", body=b)

    elif SCENARIO == "discard":
        s, _, b, ms = dev.call("DELETE", f"/network/transactions/{REST[0]}", None, {"Idempotency-Key": key()})
        step("discard", status=s, ms=ms, body=b)
        transaction(dev, REST[0])

    elif SCENARIO == "reboot-staged":
        cfg = current(dev)
        s, txn = stage(dev, candidate(cfg, dns={"mode": "manual", "servers": ["9.9.9.9"]}))
        jlink_reset(dev)
        up = dev.wait_up(90); step("board answers after reset", after_s=up)
        dev.login()
        transaction(dev, txn["id"])
        step("config after", body=current(dev))

    elif SCENARIO == "reboot-awaiting":
        txn, job, new = to_static(dev, REST[0])
        if new.wait_up(5) is not None:
            new.login(); transaction(new, txn["id"])
            jlink_reset(new)
        else:
            jlink_reset(dev)
        back = Device(DHCP_HOST)
        up = back.wait_up(90); step("DHCP address answers after reset", after_s=up)
        step("static address after reset", answers=Device(REST[0]).wait_up(3) is not None)
        back.login()
        transaction(back, txn["id"])
        step("config after", body=current(back))

    else:
        sys.exit(f"unknown scenario {SCENARIO}")
finally:
    stop_console.set(); time.sleep(0.3)
    OUT.write_text(json.dumps(out, indent=2, ensure_ascii=False))
    if CONSOLE:
        Path(CONSOLE).write_text("\n".join(console_lines) + "\n")
    print(f"-> {OUT}" + (f", console {CONSOLE}" if CONSOLE else ""))
