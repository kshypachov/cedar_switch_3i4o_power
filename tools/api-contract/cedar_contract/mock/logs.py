# SPDX-License-Identifier: Apache-2.0
"""Two bounded rings of records with cursor paging, as the device's log-store.

P1 kept the ring static and said its hard parts belonged to `log-store`'s sim
suite. P5 wrote that store, and the frontend has to meet every answer it gives,
so the mock now follows the device where the device is stricter (the rules are
tabled in the README):

- **Two rings, one sequence.** STM32 and ESP32 records live in separate bounded
  rings (`scenario.log_ring_records` each) and share one `seq`, so `source=all`
  has a stable order. Records arrive over clock time
  (`scenario.log_rate_per_s`), so a ring can wrap and a gap can be reached.
- **The cursor is bound to the boot and to the filters, separately.** A cursor
  for other filters, an empty one, or one that does not decode is
  `400 invalid_cursor`; a well-formed cursor of another boot is answered like no
  cursor at all - the newest `limit` records - with the current `boot_id` and
  `gap=true`, as the contract says.
- **A page is bounded three ways**: `limit` records, the device's response
  buffer (`LOG_PAGE_BYTES` of JSON), and records looked at per request
  (`scenario.log_scan_budget`). A page that stops early says `has_more=true`,
  and its cursor is the next record to look at - even when nothing matched.
- **`dropped_count` is records lost before a ring**, not records the ring
  overwrote; an overwrite is what `gap` reports.
- **The export is the device's**: NDJSON of LogRecord or text with a header
  line, a `kind="gap"` record where part of the snapshot was overwritten while
  it was being sent (`scenario.log_export_lost` stands in for that race, which a
  mock that builds the body at once cannot have).
"""

from __future__ import annotations

import base64
import hashlib
import json
from collections import deque
from collections.abc import Callable
from typing import Any

from ..errors import error
from .clock import Clock
from .constants import LOG_CURSOR_RESERVE, LOG_PAGE_BYTES, LOG_PAGE_RECORDS
from .scenario import Scenario

SOURCES = ("stm32", "esp32")
LEVEL_ORDER = ("debug", "info", "warning", "error")
GAP_MESSAGE = "records were overwritten before they could be exported"

#: What the device calls the reason `esp32_logs` is unavailable, by UART owner.
LOGS_UNAVAILABLE_REASON = {
    "usb_bridge": "uart_usb_bridge",
    "flashing": "uart_flashing",
    "unavailable": "uart_unavailable",
}

_ASCII_FOLD = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def ascii_lower(text: str) -> str:
    """The device folds A-Z only; `str.lower()` would also fold `É`."""
    return text.translate(_ASCII_FOLD)


def _dumps(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


class Logs:
    """Sources, a filtered page under a cursor, and a bounded export."""

    def __init__(
        self,
        clock: Clock,
        boot_id: str,
        scenario: Scenario,
        uart_mode: Callable[[], str],
        generation: Callable[[], int],
    ) -> None:
        self._clock = clock
        self._boot_id = boot_id
        self._scenario = scenario
        self._uart_mode = uart_mode
        self._generation = generation
        self._boot_ms = clock.now_ms()
        self._rings: dict[str, deque[dict[str, object]]] = {s: deque() for s in SOURCES}
        #: Highest seq each ring has overwritten; 0 while it has overwritten none.
        self._overwritten: dict[str, int] = {s: 0 for s in SOURCES}
        #: Records lost before they reached a ring (log core, UART overflow).
        self.lost: dict[str, int] = {"stm32": 0, "esp32": 17}
        self._next_seq = 1
        self._produced = 0
        self._last_settle_ms = clock.now_ms()
        self._carry = 0.0
        for index in range(45):
            source, level, module, message, kind = _TEMPLATE[index % len(_TEMPLATE)]
            self._append(
                source,
                level,
                module,
                message if index != 20 else message + " " + "x" * 64,
                kind,
                uptime_ms=1200 + index * 137,
                truncated=index == 20,
            )

    # -- the rings -----------------------------------------------------------

    def settle(self) -> None:
        """Append what `log_rate_per_s` says arrived since the last request."""
        now = self._clock.now_ms()
        elapsed = now - self._last_settle_ms
        self._last_settle_ms = now
        rate = float(self._scenario.log_rate_per_s)
        if rate <= 0 or elapsed <= 0:
            self._carry = 0.0
            self._trim()
            return
        self._carry += elapsed * rate / 1000.0
        due = int(self._carry)
        self._carry -= due
        # A skip of hours at a high rate only needs to wrap the rings, not to
        # produce every record in between.
        bound = 2 * int(self._scenario.log_ring_records) + len(_TEMPLATE)
        for _ in range(min(due, bound)):
            source, level, module, message, kind = _TEMPLATE[self._produced % len(_TEMPLATE)]
            self._append(source, level, module, f"{message} #{self._produced}", kind)
        self._trim()

    def _append(
        self,
        source: str,
        level: str | None,
        module: str | None,
        message: str,
        kind: str,
        *,
        uptime_ms: int | None = None,
        truncated: bool = False,
    ) -> None:
        record: dict[str, object] = {
            "source": source,
            "boot_id": self._boot_id,
            "source_generation": 0 if source == "stm32" else self._generation(),
            "seq": str(self._next_seq),
            "uptime_ms": str(
                uptime_ms if uptime_ms is not None else self._clock.now_ms() - self._boot_ms
            ),
            "wall_time": None,
            "level": level,
            "module": module,
            "message": message,
            "truncated": truncated,
            "kind": kind,
        }
        self._next_seq += 1
        self._produced += 1
        self._rings[source].append(record)
        self._trim()

    def _trim(self) -> None:
        cap = max(1, int(self._scenario.log_ring_records))
        for source, ring in self._rings.items():
            while len(ring) > cap:
                self._overwritten[source] = int(ring.popleft()["seq"])

    def _merged(self, sources: tuple[str, ...]) -> list[dict[str, object]]:
        records = [record for source in sources for record in self._rings[source]]
        records.sort(key=lambda record: int(record["seq"]))
        return records

    # -- reads -----------------------------------------------------------------

    def esp32_availability(self) -> tuple[bool, str | None]:
        reason = LOGS_UNAVAILABLE_REASON.get(self._uart_mode())
        return reason is None, reason

    def sources_json(self) -> dict[str, object]:
        available, reason = self.esp32_availability()
        return {
            "items": [
                {
                    "id": "stm32",
                    "available": True,
                    "reason": None,
                    "generation": 0,
                    "dropped_count": str(self.lost["stm32"]),
                },
                {
                    "id": "esp32",
                    "available": available,
                    "reason": reason,
                    "generation": self._generation(),
                    "dropped_count": str(self.lost["esp32"]),
                },
            ]
        }

    def page(self, query: dict[str, Any]) -> dict[str, object]:
        sources = _sources(query)
        match = _matcher(query)
        limit = int(query.get("limit", LOG_PAGE_RECORDS))
        budget = max(1, int(self._scenario.log_scan_budget))
        records = self._merged(sources)
        gap = False

        cursor = query.get("cursor")
        if cursor is None:
            start = _tail_start(records, match, limit, budget)
        else:
            next_seq = self._decode_cursor(cursor, query)
            if next_seq is None:
                gap = True
                start = _tail_start(records, match, limit, budget)
            else:
                gap = any(0 < self._overwritten[s] >= next_seq for s in sources)
                start = next(
                    (i for i, record in enumerate(records) if int(record["seq"]) >= next_seq),
                    len(records),
                )

        dropped = str(sum(self.lost[s] for s in sources))
        envelope = {
            "boot_id": self._boot_id,
            "items": [],
            "next_cursor": "x" * LOG_CURSOR_RESERVE,
            "has_more": False,
            "gap": False,
            "dropped_count": dropped,
        }
        size = len(_dumps(envelope))
        items: list[dict[str, object]] = []
        looked = 0
        index = start
        while index < len(records) and len(items) < limit and looked < budget:
            record = records[index]
            if match(record):
                cost = len(_dumps(record)) + (1 if items else 0)
                if size + cost > LOG_PAGE_BYTES:
                    break
                items.append(dict(record))
                size += cost
            looked += 1
            index += 1

        resume = int(records[index]["seq"]) if index < len(records) else self._next_seq
        return {
            "boot_id": self._boot_id,
            "items": items,
            "next_cursor": self._encode_cursor(resume, query),
            "has_more": index < len(records),
            "gap": gap,
            "dropped_count": dropped,
        }

    def export(self, query: dict[str, Any]) -> tuple[str, bytes]:
        sources = _sources(query)
        match = _matcher(query)
        limit = int(query.get("max_records", 2000))
        records = self._merged(sources)
        start = _tail_start(records, match, limit, len(records) + 1)
        snapshot = [record for record in records[start:] if match(record)][-limit:]

        lost = max(0, int(self._scenario.log_export_lost))
        lines: list[dict[str, object]] = []
        if lost and snapshot:
            gone, snapshot = snapshot[:lost], snapshot[lost:]
            after = snapshot[0] if snapshot else None
            lines.append(
                {
                    "source": gone[0]["source"],
                    "boot_id": self._boot_id,
                    "source_generation": gone[0]["source_generation"],
                    "seq": after["seq"] if after is not None else str(self._next_seq),
                    "uptime_ms": str(self._clock.now_ms() - self._boot_ms),
                    "wall_time": None,
                    "level": None,
                    "module": None,
                    "message": GAP_MESSAGE,
                    "truncated": False,
                    "kind": "gap",
                }
            )
        lines.extend(dict(record) for record in snapshot)

        if query.get("format", "ndjson") == "text":
            text = [
                f"# cedar logs {self._boot_id}: bounded snapshot, nothing from before this boot;"
                " gaps are marked"
            ]
            for record in lines:
                if record["kind"] == "gap":
                    text.append(f"# gap: {GAP_MESSAGE}")
                    continue
                message = str(record["message"]).replace("\r\n", "\\n")
                message = message.replace("\r", "\\n").replace("\n", "\\n")
                text.append(
                    f"{record['uptime_ms']} {record['source']} {record['level'] or '-'} "
                    f"{record['module'] or '-'} {message}"
                )
            return "text/plain; charset=utf-8", ("\n".join(text) + "\n").encode()
        return "application/x-ndjson", b"".join(_dumps(record) + b"\n" for record in lines)

    # -- the cursor ------------------------------------------------------------

    def _encode_cursor(
        self, next_seq: int, query: dict[str, Any], boot_id: str | None = None
    ) -> str:
        body = f"1:{_tag(boot_id or self._boot_id)}:{_filter_hash(query)}:{next_seq}"
        token = f"{body}:{_check(body)}"
        return base64.urlsafe_b64encode(token.encode()).decode().rstrip("=")

    def _decode_cursor(self, cursor: str, query: dict[str, Any]) -> int | None:
        """The sequence to resume at, or None for a cursor of another boot."""
        padded = cursor + "=" * (-len(cursor) % 4)
        try:
            version, boot, filters, seq_text, check = (
                base64.urlsafe_b64decode(padded).decode().split(":")
            )
            next_seq = int(seq_text)
        except (ValueError, TypeError, UnicodeDecodeError):
            raise error("invalid_cursor", "The cursor is not one this device issued") from None
        body = f"{version}:{boot}:{filters}:{seq_text}"
        if version != "1" or check != _check(body) or next_seq < 0:
            raise error("invalid_cursor", "The cursor is not one this device issued")
        if boot != _tag(self._boot_id):
            return None
        if filters != _filter_hash(query):
            raise error(
                "invalid_cursor",
                "The cursor belongs to a different filter; drop it when filters change",
            )
        if next_seq > self._next_seq:
            raise error("invalid_cursor", "The cursor points past the newest record")
        return next_seq


def _sources(query: dict[str, Any]) -> tuple[str, ...]:
    source = query.get("source", "all")
    return SOURCES if source == "all" else (source,)


def _matcher(query: dict[str, Any]) -> Callable[[dict[str, object]], bool]:
    min_level = query.get("min_level")
    module = query.get("module")
    contains = query.get("contains")
    floor = LEVEL_ORDER.index(min_level) if min_level in LEVEL_ORDER else None
    needle = ascii_lower(contains) if contains is not None else None

    def match(record: dict[str, object]) -> bool:
        if module is not None and record["module"] != module:
            return False
        if needle is not None and needle not in ascii_lower(str(record["message"])):
            return False
        if floor is not None:
            level = record["level"]
            # `unknown` and null survive a min_level filter: a level the device
            # could not parse is not thereby less important than one it could.
            if level in LEVEL_ORDER and LEVEL_ORDER.index(level) < floor:
                return False
        return True

    return match


def _tail_start(
    records: list[dict[str, object]],
    match: Callable[[dict[str, object]], bool],
    count: int,
    budget: int,
) -> int:
    """Walk back from the newest record, looking at no more than `budget`, until
    `count` records have matched; the page starts there."""
    found = 0
    index = len(records)
    looked = 0
    while index > 0 and found < count and looked < budget:
        index -= 1
        looked += 1
        if match(records[index]):
            found += 1
    return index


def _tag(boot_id: str) -> str:
    return hashlib.sha256(boot_id.encode()).hexdigest()[:8]


def _filter_hash(query: dict[str, Any]) -> str:
    parts = "|".join(
        [
            str(query.get("source", "all")),
            *(str(query.get(key, "")) for key in ("min_level", "module", "contains")),
        ]
    )
    return hashlib.sha256(parts.encode()).hexdigest()[:12]


def _check(body: str) -> str:
    return hashlib.sha256(f"cedar-log-cursor|{body}".encode()).hexdigest()[:8]


#: Enough records, and enough awkward ones, to exercise the log screen: a null
#: level from early boot, a `reset` marker for the C6, a `paused` marker for the
#: flashing window, raw ANSI, and (at index 20 of the seed) a truncated line.
_TEMPLATE: list[tuple[str, str | None, str | None, str, str]] = [
    ("stm32", None, None, "*** Booting Zephyr OS build v4.4.0 ***", "message"),
    ("stm32", "info", "mcuboot", "Bootloader chainload address offset: 0x0", "message"),
    ("stm32", "info", "psram", "PSRAM mapped at 0x90000000, 8 MiB", "message"),
    ("stm32", "info", "net_if", "Ethernet link up, requesting DHCP", "message"),
    ("stm32", "info", "net_dhcpv4", "Received: 192.168.88.14", "message"),
    ("esp32", None, None, "ESP-ROM:esp32c6-20220919", "reset"),
    ("esp32", "info", "wifi", "wifi driver task: start", "message"),
    ("esp32", "warning", "wifi", "AP not found, retrying in 5s", "message"),
    ("stm32", "warning", "eth_w5500", "receive queue high water mark 12", "message"),
    ("stm32", "error", "settings", "settings_registry: 0 keys registered", "message"),
    ("esp32", "info", "esp_hosted", "transport ready, protocol esp-hosted-mcu-3", "message"),
    ("stm32", "debug", "matter", "FabricTable: 0 fabrics loaded", "message"),
    ("esp32", "unknown", None, "\x1b[0;32mI (412) boot: compile time 11:22:33", "message"),
    ("stm32", "info", "web", "HTTP server listening on 0.0.0.0:80", "message"),
    ("esp32", None, None, "UART handed to the flasher", "paused"),
]
