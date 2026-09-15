"""Board B only: what every P6 bench script shares.

Board B: J-Link 000941000024, console /dev/cu.usbmodem5AE60208891 (WCH bridge, serial
5AE6020889), application CDC with USB serial 2037394C3543501200210047 (one CDC ACM
function, interface 0 "Zephyr USB CDC-ACM uart0"), DHCP 192.168.88.13.

Board A belongs to another session. Its address, ports and serials are refused before
anything is opened or sent; no other address than board B's is ever contacted.
"""
import http.client
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import serial
import serial.tools.list_ports

BOARD_A_REFUSED = ("192.168.88.14", "002F002B3233510739363634", "2037394C3543501200550045")
# Ethernet DHCP address by default; P6_HOST=192.168.88.23 when the owner leaves only Wi-Fi
# (2026-09-14, C6 flashed). Board A's address is refused below either way.
HOST = os.environ.get("P6_HOST", "192.168.88.13")
# Set by the owner through the setup page after the ZMS settings wipe (2026-09-14 ~17:30);
# before it was "cedar-bench-P4-boardB".
PASSWORD = os.environ.get("P6_PASSWORD", "111111111111")
CONSOLE = "/dev/cu.usbmodem5AE60208891"
CDC_SERIAL = "2037394C3543501200210047"
JLINK = ["JLinkExe", "-USB", "000941000024", "-device", "STM32U585AI", "-if", "SWD",
         "-speed", "4000", "-autoconnect", "1", "-NoGui", "1", "-ExitOnError", "1"]
# hw -> p6 -> reports -> device-development -> docs -> the repository
REPO = Path(__file__).resolve().parents[5]
DOC = json.loads((REPO / "docs/device-development/openapi.json").read_text())


def refuse(value):
    """Anything that names board A stops the script before it touches anything."""
    for bad in BOARD_A_REFUSED:
        if bad in str(value):
            sys.exit(f"{value}: names board A; refusing")


refuse(HOST)
refuse(CONSOLE)


# ---- clock and a record of steps -------------------------------------------------

T0 = time.time()


def now():
    return round(time.time() - T0, 2)


class Record:
    """Steps with host time, written as JSON; console lines with the same clock."""

    def __init__(self, out):
        self.out = Path(out)
        self.steps = []

    def step(self, what, **data):
        entry = {"t": now(), "step": what, **data}
        self.steps.append(entry)
        print(json.dumps(entry, ensure_ascii=False)[:500], flush=True)
        if CONSOLE_READER.running:
            CONSOLE_READER.mark(what)
        return entry

    def save(self, **extra):
        body = {"steps": self.steps, **extra}
        if IMAGE_SHA:
            body["image_sha256"] = IMAGE_SHA
        self.out.write_text(json.dumps(body, ensure_ascii=False, indent=1))


IMAGE_SHA = os.environ.get("P6_IMAGE_SHA256")


# ---- the console -----------------------------------------------------------------

class Console:
    """One reader of board B's console; shell commands wait for their answer."""

    def __init__(self):
        self.lines = []
        self.partial = ""
        self.lock = threading.Lock()
        self.running = False
        self.port = None

    def start(self):
        refuse(CONSOLE)
        self.port = serial.Serial()
        self.port.port, self.port.baudrate, self.port.timeout = CONSOLE, 115200, 0.1
        self.port.dtr = self.port.rts = False
        self.port.open()
        self.running = True
        threading.Thread(target=self._read, daemon=True).start()
        time.sleep(0.3)

    def _read(self):
        while self.running:
            try:
                chunk = self.port.read(4096)
            except serial.SerialException:
                time.sleep(0.2)
                continue
            if not chunk:
                continue
            text = chunk.decode("utf-8", "replace").replace("\r", "")
            with self.lock:
                self.partial += text
                *done, self.partial = self.partial.split("\n")
                self.lines.extend((now(), line) for line in done)

    def mark(self, what):
        with self.lock:
            self.lines.append((now(), f"==== host: {what}"))

    def shell(self, command, expect, timeout=5.0, retries=3):
        """Send @p command; return the first line matching @p expect after it, or None.

        The console loses bytes now and then (P0): a command whose answer does not
        come is sent again."""
        pattern = re.compile(expect)
        for _ in range(retries):
            with self.lock:
                start = len(self.lines)
            self.port.write((command + "\r\n").encode())
            deadline = time.time() + timeout
            while time.time() < deadline:
                with self.lock:
                    fresh = self.lines[start:] + [(now(), self.partial)]
                for _, line in fresh:
                    if pattern.search(line):
                        return line
                time.sleep(0.05)
        return None

    def since(self, t):
        with self.lock:
            return [line for stamp, line in self.lines if stamp >= t]

    def save(self, path):
        with self.lock:
            text = "\n".join(f"[+{stamp:8.2f}] {line}" for stamp, line in self.lines)
        Path(path).write_text(text + "\n")

    def stop(self):
        self.running = False
        time.sleep(0.2)
        if self.port:
            self.port.close()


CONSOLE_READER = Console()


# ---- HTTP ------------------------------------------------------------------------

class Device:
    def __init__(self):
        self.cookie = None
        self.csrf = None

    def call(self, method, path, body=None, timeout=15):
        refuse(HOST)
        headers = {"Host": HOST, "Origin": f"http://{HOST}"}
        if self.cookie:
            headers["Cookie"] = self.cookie
        if self.csrf and method != "GET":
            headers["X-CSRF-Token"] = self.csrf
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        c = http.client.HTTPConnection(HOST, 80, timeout=timeout)
        t = time.monotonic()
        c.request(method, "/api/v1" + path, body=data, headers=headers)
        r = c.getresponse()
        raw = r.read()
        ms = round((time.monotonic() - t) * 1000)
        hdrs = {k.lower(): v for k, v in r.getheaders()}
        c.close()
        try:
            parsed = json.loads(raw) if raw and "json" in hdrs.get("content-type", "") else raw
        except ValueError:
            parsed = raw
        return r.status, hdrs, parsed, ms

    def login(self):
        s, h, b, ms = self.call("POST", "/auth/session", {"password": PASSWORD})
        if s != 200:
            raise RuntimeError(f"login: {s} {b}")
        self.cookie = h["set-cookie"].split(";")[0]
        self.csrf = b["csrf_token"]
        return ms

    def wait_up(self, limit):
        t = time.monotonic()
        while time.monotonic() - t < limit:
            try:
                s, _, _, _ = self.call("GET", "/auth/state", timeout=3)
                if s == 200:
                    return round(time.monotonic() - t, 1)
            except OSError:
                pass
            time.sleep(1.0)
        return None


def raw_get(path, cookie, read=True, timeout=30, max_bytes=None, rcvbuf=None):
    """A GET on a plain socket: the whole response, chunk sizes, time to the end.

    @p rcvbuf shrinks the host's receive buffer before connecting, so a client that
    stops reading closes its TCP window within a few kilobytes - otherwise the host
    kernel quietly accepts hundreds of kilobytes on its behalf."""
    refuse(HOST)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(timeout)
    s.connect((HOST, 80))
    s.sendall(f"GET /api/v1{path} HTTP/1.1\r\nHost: {HOST}\r\nCookie: {cookie}\r\n\r\n".encode())
    if not read:
        return s
    data = bytearray()
    t = time.monotonic()
    while True:
        try:
            part = s.recv(65536)
        except socket.timeout:
            break
        if not part:
            break
        data.extend(part)
        if data.endswith(b"0\r\n\r\n") or (max_bytes and len(data) >= max_bytes):
            break
    s.close()
    return bytes(data), round(time.monotonic() - t, 3)


def dechunk(raw):
    head, _, body = raw.partition(b"\r\n\r\n")
    out, sizes = bytearray(), []
    while body:
        line, _, rest = body.partition(b"\r\n")
        try:
            n = int(line, 16)
        except ValueError:
            break
        if n == 0:
            break
        out.extend(rest[:n])
        sizes.append(n)
        body = rest[n + 2:]
    return head.decode(errors="replace"), bytes(out), sizes


# ---- schema ------------------------------------------------------------------------

from jsonschema import Draft202012Validator  # noqa: E402
from referencing import Registry, Resource  # noqa: E402
from referencing.jsonschema import DRAFT202012  # noqa: E402

_REGISTRY = Registry().with_resource(
    "doc", Resource.from_contents({"components": DOC["components"]}, default_specification=DRAFT202012))


def schema_errors(name, body):
    v = Draft202012Validator({"$ref": f"doc#/components/schemas/{name}"}, registry=_REGISTRY,
                             format_checker=Draft202012Validator.FORMAT_CHECKER)
    return [f"{list(e.absolute_path)}: {e.message}" for e in v.iter_errors(body)]


# ---- board ---------------------------------------------------------------------------

def jlink(commands):
    with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as f:
        f.write(commands + "q\n")
        script = f.name
    r = subprocess.run(JLINK + ["-CommanderScript", script], capture_output=True, text=True)
    os.unlink(script)
    return r


def cdc_port():
    """Board B's application CDC, by its exact USB serial."""
    for p in serial.tools.list_ports.comports():
        if p.serial_number == CDC_SERIAL:
            refuse(p.device)
            return p.device
    return None


def ping_once(timeout_ms=1500):
    refuse(HOST)
    r = subprocess.run(["ping", "-c", "1", "-W", str(timeout_ms), HOST], capture_output=True, text=True)
    m = re.search(r"time=([0-9.]+) ms", r.stdout)
    return float(m.group(1)) if m else None
