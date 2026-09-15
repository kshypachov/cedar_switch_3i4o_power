"""Board B only: P6 step 2 - esp-loader-adapter on the board with nothing written to the C6.
`coproc loader` opens a session (UART to `flashing`, ROM loader, connect, ESP32-C6 check)
and closes it (normal boot, EN/BOOT idle, console configuration, UART back to the console).
No write, erase or begin command exists in that path.

usage: loader_b.py <scenario> <out prefix> [args]
  check           status before, `coproc loader`, status after; the ESP32 ring receives ROM output again
  repeat <n>      n sessions 3 s apart: every open and close succeeds, no fault, console back each time
  bridge          the USB bridge owns the UART: `coproc loader` is refused; console back afterwards

Shell commands go over telnet (the serial console loses input bytes); the serial console is
recorded for faults and timing. Writes <prefix>.json and <prefix>.console.log.
"""
import re
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
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


class Shell:
    """Zephyr's telnet shell: send a line, collect output until @p expect or the timeout."""

    def __init__(self):
        b.refuse(b.HOST)
        self.sock = socket.create_connection((b.HOST, 23), timeout=5)
        self.sock.settimeout(0.2)
        self.buf = ""
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()
        time.sleep(0.5)

    def _read(self):
        while True:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            # Telnet negotiation (IAC ...) is dropped; the shell prints plain text.
            text = bytes(x for x in data if x < 0xF0).decode("utf-8", "replace")
            with self.lock:
                self.buf += ANSI.sub("", text).replace("\r", "")

    def run(self, command, expect, timeout=10.0):
        with self.lock:
            self.buf = ""
        self.sock.sendall((command + "\r\n").encode())
        pattern = re.compile(expect)
        t = time.monotonic()
        while time.monotonic() - t < timeout:
            with self.lock:
                text = self.buf
            if pattern.search(text):
                time.sleep(0.2)
                with self.lock:
                    text = self.buf
                return [l for l in text.split("\n") if l.strip() and not l.strip().endswith("$")]
            time.sleep(0.05)
        with self.lock:
            return ["TIMEOUT"] + [l for l in self.buf.split("\n") if l.strip()]

    def close(self):
        self.sock.close()


def esp32_tail():
    s, _, body, _ = dev.call("GET", "/logs/records?source=esp32&limit=1")
    return (body.get("items") or [None])[-1] if s == 200 else None, body.get("next_cursor") if s == 200 else None


def esp32_since(cursor):
    s, _, body, _ = dev.call("GET", f"/logs/records?source=esp32&limit=100&cursor={cursor}")
    return len(body.get("items", [])) if s == 200 else None


def status():
    s, _, body, ms = dev.call("GET", "/coprocessor/status")
    return {k: body.get(k) for k in ("state", "uart_mode", "generation", "transport_ready")} if s == 200 else s


def loader_once(sh, label):
    before = status()
    _, cursor = esp32_tail()
    lines = sh.run("coproc loader", r"coproc loader: uart_mode", timeout=40)
    after = status()
    time.sleep(3)
    rec.step(label, before=before, lines=lines, after=after, esp32_records_3s_after=esp32_since(cursor),
             faults=[l for l in con.since(0) if "FAULT" in l or "ASSERT" in l])
    return any("close 0" in l for l in lines)


def main():
    con.start()
    up = dev.wait_up(90)
    rec.step("http up", seconds=up)
    if up is None:
        return
    rec.step("login", ms=dev.login())
    sh = Shell()
    rec.step("coproc status", lines=sh.run("coproc status", r"lines \d+"))
    if SCENARIO == "check":
        loader_once(sh, "loader")
    elif SCENARIO == "repeat":
        n = int(ARGS[0])
        ok = sum(loader_once(sh, f"loader {i + 1}") for i in range(n))
        rec.step("repeat summary", sessions=n, closed_ok=ok)
    elif SCENARIO == "bridge":
        rec.step("bridge on", lines=sh.run("coproc mode bridge", r"coproc mode bridge:"))
        rec.step("status in bridge", status=status())
        rec.step("loader in bridge", lines=sh.run("coproc loader", r"coproc loader:", timeout=20))
        rec.step("console back", lines=sh.run("coproc mode console", r"coproc mode console:"))
        loader_once(sh, "loader after bridge")
    else:
        sys.exit(f"unknown scenario {SCENARIO}")
    rec.step("coproc status after", lines=sh.run("coproc status", r"lines \d+"))
    sh.close()


if __name__ == "__main__":  # shell_b.py imports Shell from here
    try:
        main()
    finally:
        rec.save()
        con.save(PREFIX + ".console.log")
        con.stop()
