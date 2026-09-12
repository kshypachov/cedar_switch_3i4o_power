# SPDX-License-Identifier: Apache-2.0
"""The board's shell console, shared by the bench scripts in this directory.

Three properties matter more than convenience here, and each was paid for:

- The port is found by the ST-LINK's USB serial number, not by name. The
  /dev/cu.usbmodem* number changes between sessions (11303 one day, 21103 the
  next), and the bench has a second ST-LINK attached.
- Reading never stops. The VCP disappears for a moment on every reset; the
  reader reopens it and keeps going, so a boot is captured from its first
  line instead of the capture dying silently on the first reset.
- Every line carries the host time it arrived. Boot timings and shell latency
  are computed from those stamps, so they are only as good as the read loop,
  which polls every 10 ms.

A console that stops answering is a result, not an error: W5500 starvation
shows up exactly as a shell that goes quiet while ping still works. Callers
get None back and decide what that means.
"""

import plistlib
import re
import subprocess
import threading
import time

import serial

ST_LINK_SERIAL = "002F002B3233510739363634"
BAUD = 115200

_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def resolve_port(usb_serial):
    """Return the /dev/cu.* callout device of the USB device with this serial."""
    tree = plistlib.loads(
        subprocess.run(
            ["ioreg", "-a", "-r", "-l", "-c", "IOUSBHostDevice"],
            capture_output=True,
            check=True,
        ).stdout
    )

    def callout(node):
        if "IOCalloutDevice" in node:
            return node["IOCalloutDevice"]
        for child in node.get("IORegistryEntryChildren", []):
            found = callout(child)
            if found:
                return found
        return None

    def walk(node):
        if node.get("USB Serial Number") == usb_serial:
            found = callout(node)
            if found:
                return found
        for child in node.get("IORegistryEntryChildren", []):
            found = walk(child)
            if found:
                return found
        return None

    for root in tree:
        found = walk(root)
        if found:
            return found
    raise LookupError(f"no USB serial device with serial {usb_serial}")


class Console:
    """Background reader plus a writer, over a port that comes and goes."""

    def __init__(self, raw_log_path, usb_serial=ST_LINK_SERIAL):
        self.usb_serial = usb_serial
        self._raw = open(raw_log_path, "ab")
        self._ser = None
        self._write_lock = threading.Lock()
        self._cond = threading.Condition()
        self._lines = []  # (host time, text without ANSI and CR)
        self._stop = False
        self.reopen_count = 0
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def close(self):
        self._stop = True
        self._thread.join(timeout=2)
        self._raw.close()

    # -- reading ---------------------------------------------------------

    def _append(self, now, raw_line):
        text = _ANSI.sub("", raw_line.decode(errors="replace")).rstrip("\r")
        self._lines.append((now, text))

    def _note(self, text):
        now = time.time()
        self._raw.write(f"\n[bench {time.strftime('%H:%M:%S')}: {text}]\n".encode())
        self._raw.flush()
        with self._cond:
            self._lines.append((now, f"[bench: {text}]"))
            self._cond.notify_all()

    def _run(self):
        partial = b""
        while not self._stop:
            try:
                port = resolve_port(self.usb_serial)
                with serial.Serial(port, BAUD, timeout=0.01) as ser:
                    self._ser = ser
                    while not self._stop:
                        data = ser.read(4096)
                        if not data:
                            continue
                        now = time.time()
                        self._raw.write(data)
                        self._raw.flush()
                        partial += data
                        *complete, partial = partial.split(b"\n")
                        if complete:
                            with self._cond:
                                for line in complete:
                                    self._append(now, line)
                                self._cond.notify_all()
            except (serial.SerialException, OSError, LookupError,
                    subprocess.CalledProcessError) as err:
                self._ser = None
                self.reopen_count += 1
                self._note(f"console lost ({err}); reopening")
                time.sleep(0.3)

    def mark(self):
        """Index to pass to wait_for() so only lines after this point match."""
        with self._cond:
            return len(self._lines)

    def lines_since(self, index):
        with self._cond:
            return list(self._lines[index:])

    def wait_for(self, pattern, since, timeout):
        """First line after `since` matching `pattern`: (time, match, index) or None."""
        regex = re.compile(pattern)
        deadline = time.time() + timeout
        scanned = since
        with self._cond:
            while True:
                while scanned < len(self._lines):
                    t, text = self._lines[scanned]
                    m = regex.search(text)
                    if m:
                        return t, m, scanned
                    scanned += 1
                left = deadline - time.time()
                if left <= 0:
                    return None
                self._cond.wait(left)

    # -- writing ---------------------------------------------------------

    def send(self, command):
        """Write one shell command. Returns the host time of the write or None."""
        with self._write_lock:
            ser = self._ser
            if ser is None:
                return None
            try:
                t = time.time()
                ser.write(command.encode() + b"\r\n")
                return t
            except (serial.SerialException, OSError):
                return None

    def probe(self, command, answer_pattern, timeout):
        """Send a command and time its answer.

        Returns (latency seconds, match) or (None, None) if no answer came.
        """
        since = self.mark()
        sent = self.send(command)
        if sent is None:
            return None, None
        hit = self.wait_for(answer_pattern, since, timeout)
        if hit is None:
            return None, None
        return hit[0] - sent, hit[1]

    def collect(self, command, quiet=0.8, timeout=10.0):
        """Send a command and return the text that follows until output goes quiet."""
        since = self.mark()
        if self.send(command) is None:
            return None
        deadline = time.time() + timeout
        seen = since
        last_change = time.time()
        while time.time() < deadline:
            time.sleep(0.05)
            count = self.mark()
            if count != seen:
                seen = count
                last_change = time.time()
            elif time.time() - last_change >= quiet and count > since:
                break
        return "\n".join(text for _, text in self.lines_since(since))
