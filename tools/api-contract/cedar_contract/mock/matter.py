# SPDX-License-Identifier: Apache-2.0
"""The commissioning window, its codes, and the fabric table.

Two contract rules shape this. Opening a window that is already open is
`409 invalid_state` rather than a silent replacement, because the open window
may belong to another administrator. And with no window open the codes resource
answers `available=false` with a reason and three nulls — it never falls back to
a factory QR, which would send someone the wrong passcode.
"""

from __future__ import annotations

import hashlib

from ..errors import error
from .clock import Clock
from .constants import MATTER_CLOSE_MS, MATTER_OPEN_MS, QUEUE_MS
from .jobs import JobStore, Step
from .scenario import Scenario
from .util import detail


class Matter:
    """The commissioning window, its codes, and the fabric table."""

    #: Fixed mock constants, not device codes. The contract forbids inventing
    #: random test QR payloads, so these are stable and obviously synthetic; a
    #: real device gets them from the Matter SDK.
    QR_PAYLOAD = "MT:Y3.13OTB00KA0648G00"
    MANUAL_CODE = "34970112332"
    SETUP_PASSCODE = "20202021"

    def __init__(self, clock: Clock, jobs: JobStore, scenario: Scenario) -> None:
        self._clock = clock
        self._jobs = jobs
        self._scenario = scenario
        self._open = False
        self._mode: str | None = None
        self._source: str | None = None
        self._opened_ms = 0
        self._timeout_s = 0
        self._fabrics = [_fabric(i) for i in range(scenario.fabrics)]

    def settle(self) -> None:
        if self._open and self.remaining_seconds() == 0:
            self._close()

    def remaining_seconds(self) -> int:
        if not self._open:
            return 0
        elapsed = (self._clock.now_ms() - self._opened_ms) // 1000
        return max(0, self._timeout_s - elapsed)

    def status_json(self) -> dict[str, object]:
        self.settle()
        return {
            "state": self._scenario.matter_state,
            "commissioned": bool(self._fabrics),
            "fabric_count": len(self._fabrics),
            "error": None
            if self._scenario.matter_state != "failed"
            else detail(error("service_not_ready", "The Matter stack failed to start")),
        }

    def window_json(self) -> dict[str, object]:
        self.settle()
        return {
            "open": self._open,
            "mode": self._mode,
            "source": self._source,
            "remaining_seconds": self.remaining_seconds(),
            "codes_available": self._open and self._mode == "basic",
        }

    def codes_json(self) -> dict[str, object]:
        self.settle()
        if not self._open:
            return _no_codes("window_closed")
        if self._mode != "basic":
            # An enhanced window opened by an external controller may carry a
            # verifier whose passcode this device never knew.
            return _no_codes("passcode_unavailable")
        return {
            "available": True,
            "reason": None,
            "qr_payload": self.QR_PAYLOAD,
            "manual_pairing_code": self.MANUAL_CODE,
            "setup_passcode": self.SETUP_PASSCODE,
        }

    def fabrics_json(self) -> dict[str, object]:
        return {"items": [dict(f) for f in self._fabrics], "count": len(self._fabrics)}

    def open_window(self, mode: str, timeout_seconds: int) -> str:
        self.settle()
        self._require_ready()
        if self._open:
            raise error(
                "invalid_state",
                "A commissioning window is already open; close it before opening another",
            )

        def opened() -> None:
            self._open = True
            self._mode = mode
            self._source = "web"
            self._opened_ms = self._clock.now_ms()
            self._timeout_s = timeout_seconds

        job = self._jobs.create(
            "matter_open",
            [
                Step("queued", QUEUE_MS, state="queued"),
                Step("opening", MATTER_OPEN_MS, on_done=opened),
            ],
            resource_url="/api/v1/matter/commissioning",
        )
        return job.id

    def close_window(self) -> str:
        """Closing an already closed window is idempotent, per the contract:
        still a `202`, and the job still succeeds."""
        self.settle()
        self._require_ready()
        job = self._jobs.create(
            "matter_close",
            [Step("closing", MATTER_CLOSE_MS, on_done=self._close)],
            resource_url="/api/v1/matter/commissioning",
        )
        return job.id

    def _require_ready(self) -> None:
        if self._scenario.matter_state != "ready":
            raise error(
                "service_not_ready",
                f"Matter is {self._scenario.matter_state}; commissioning is unavailable",
            )

    def _close(self) -> None:
        """A window that has been open leaves a fabric behind.

        On the device that only happens when a controller actually commissions.
        The mock does it unconditionally because it is the only way a frontend
        e2e run reaches the populated fabric table at all, and an empty table
        after a successful commissioning is the one outcome the screen must not
        show.
        """
        if self._open and len(self._fabrics) < 254:
            self._fabrics.append(_fabric(len(self._fabrics)))
        self._open = False
        self._mode = None
        self._source = None
        self._timeout_s = 0


def _no_codes(reason: str) -> dict[str, object]:
    return {
        "available": False,
        "reason": reason,
        "qr_payload": None,
        "manual_pairing_code": None,
        "setup_passcode": None,
    }


def _fabric(index: int) -> dict[str, object]:
    """An opaque id that includes the root identity, as the schema demands, and
    not the fabric index — which may be reused after a fabric is removed."""
    fabric_id = f"{0xFAB000000000AA01 + index:016X}"
    node_id = f"{0x000000000001B669 + index:016X}"
    root = hashlib.sha256(f"root/{fabric_id}".encode()).hexdigest()[:16]
    return {
        "id": f"{root}:{fabric_id}",
        "fabric_index": index + 1,
        "fabric_id": fabric_id,
        "node_id": node_id,
        "vendor_id": (0x1349, 0xFFF1, 0x101D)[index % 3],
        "label": ("", "home", "lab")[index % 3],
    }
