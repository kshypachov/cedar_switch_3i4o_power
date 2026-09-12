# SPDX-License-Identifier: Apache-2.0
"""A fixed ring of records with cursor paging that actually works.

Section 12 places logs at the coverage level, so the ring's own hard parts — a
wrap, a stale cursor, a gap — belong to `log-store`'s sim suite and are not
reproduced here. But a cursor that never advanced would make the log screen's
paging loop forever, so the arithmetic is real: filters are applied, the cursor
is opaque and bound to the filters that produced it, and a cursor that does not
match the current filters is `400 invalid_cursor` as the contract requires.
"""

from __future__ import annotations

import base64
import hashlib
import json
from typing import Any

from ..errors import error
from .clock import Clock
from .constants import LOG_PAGE_RECORDS


class Logs:
    """Sources, a filtered page under a cursor, and a bounded export."""

    def __init__(self, clock: Clock, boot_id: str) -> None:
        self._clock = clock
        self._boot_id = boot_id
        self._records = _log_records(boot_id)

    def sources_json(self) -> dict[str, object]:
        return {
            "items": [
                {
                    "id": "stm32",
                    "available": True,
                    "reason": None,
                    "generation": 1,
                    "dropped_count": "0",
                },
                {
                    "id": "esp32",
                    "available": True,
                    "reason": None,
                    "generation": 2,
                    "dropped_count": "17",
                },
            ]
        }

    def page(self, query: dict[str, Any]) -> dict[str, object]:
        matching = self._filter(query)
        limit = int(query.get("limit", LOG_PAGE_RECORDS))
        cursor = query.get("cursor")
        if cursor is None:
            # No cursor: the newest `limit` matching records, oldest first.
            window = matching[-limit:]
            start = len(matching) - len(window)
        else:
            start = self._decode_cursor(cursor, query)
            window = matching[start : start + limit]
        end = start + len(window)
        return {
            "boot_id": self._boot_id,
            "items": [dict(record) for record in window],
            "next_cursor": self._encode_cursor(end, query),
            "has_more": end < len(matching),
            "gap": False,
            "dropped_count": "17",
        }

    def export(self, query: dict[str, Any]) -> tuple[str, bytes]:
        limit = int(query.get("max_records", 2000))
        matching = self._filter(query)[-limit:]
        if query.get("format", "ndjson") == "text":
            lines = [
                "# bounded snapshot; records before the ring started are not included",
                *(
                    f"{r['uptime_ms']} {r['source']} {r['level'] or 'unknown'} "
                    f"{r['module'] or '-'} {r['message']}"
                    for r in matching
                ),
            ]
            return "text/plain; charset=utf-8", ("\n".join(lines) + "\n").encode()
        body = b"".join(
            json.dumps(record, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
            for record in matching
        )
        return "application/x-ndjson", body

    def _filter(self, query: dict[str, Any]) -> list[dict[str, object]]:
        source = query.get("source", "all")
        min_level = query.get("min_level")
        module = query.get("module")
        contains = query.get("contains")
        order = ["debug", "info", "warning", "error"]
        floor = order.index(min_level) if min_level in order else None
        out = []
        for record in self._records:
            if source != "all" and record["source"] != source:
                continue
            if module is not None and record["module"] != module:
                continue
            if contains is not None and contains.lower() not in str(record["message"]).lower():
                continue
            if floor is not None:
                level = record["level"]
                # `unknown` and null survive a min_level filter: the contract
                # says a level the device could not parse is not thereby less
                # important than one it could.
                if level in order and order.index(level) < floor:
                    continue
            out.append(record)
        return out

    def _encode_cursor(self, index: int, query: dict[str, Any]) -> str:
        token = f"{index}:{self._filter_hash(query)}"
        return base64.urlsafe_b64encode(token.encode()).decode().rstrip("=")

    def _decode_cursor(self, cursor: str, query: dict[str, Any]) -> int:
        padded = cursor + "=" * (-len(cursor) % 4)
        try:
            index_text, digest = base64.urlsafe_b64decode(padded).decode().split(":", 1)
            index = int(index_text)
        except (ValueError, TypeError):
            raise error("invalid_cursor", "The cursor is not one this device issued") from None
        if digest != self._filter_hash(query) or index < 0:
            raise error(
                "invalid_cursor",
                "The cursor belongs to a different filter; drop it when filters change",
            )
        return index

    def _filter_hash(self, query: dict[str, Any]) -> str:
        parts = "|".join(
            str(query.get(key, "")) for key in ("source", "min_level", "module", "contains")
        )
        return hashlib.sha256(f"{self._boot_id}|{parts}".encode()).hexdigest()[:12]


def _log_records(boot_id: str) -> list[dict[str, object]]:
    """Enough records, and enough awkward ones, to exercise the log screen.

    Includes a null level from early boot, a `reset` marker for the C6, a
    `paused` marker for the flashing window, and a line marked truncated.
    """
    template: list[tuple[str, str | None, str | None, str, str]] = [
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
        ("esp32", "info", "esp_hosted", "transport ready, protocol esp-hosted-mcu-2.0", "message"),
        ("stm32", "debug", "matter", "FabricTable: 0 fabrics loaded", "message"),
        ("esp32", "unknown", None, "\x1b[0;32mI (412) boot: compile time 11:22:33", "message"),
        ("stm32", "info", "web", "HTTP server listening on 0.0.0.0:80", "message"),
        ("esp32", "info", "coprocessor", "UART handed to the flasher", "paused"),
    ]
    records: list[dict[str, object]] = []
    for index in range(45):
        source, level, module, message, kind = template[index % len(template)]
        records.append(
            {
                "source": source,
                "boot_id": boot_id,
                "source_generation": 1 if source == "stm32" else 2,
                "seq": str(index + 1),
                "uptime_ms": str(1200 + index * 137),
                "wall_time": None,
                "level": level,
                "module": module,
                "message": message if index != 20 else message + " " + "x" * 64,
                "truncated": index == 20,
                "kind": kind,
            }
        )
    return records
