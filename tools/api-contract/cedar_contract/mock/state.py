# SPDX-License-Identifier: Apache-2.0
"""The whole device: every subsystem, sharing one clock and one job store.

Each subsystem lives in its own module and is a small state machine taken from
`api-contract.md`. Decisions that apply to all of them:

- **Rejections come from the contract's table, never from a judgement call at
  the call site.** Where the contract names a code for a situation, that code is
  used. The three places it names none are marked `DECISION` in the module that
  makes the choice, and listed in the README so the device can be made to match
  rather than quietly differ.
- **Nothing has a thread.** Time-driven transitions — a candidate expiring, a
  confirmation window closing, a job finishing a phase — happen in `settle()`,
  which runs once at the top of every request. So any number of pollers see one
  consistent state, and a test steps the clock instead of sleeping.
- **Secrets never come back out.** A password is compared and dropped; a
  candidate's Wi-Fi credential becomes `password_set` and nothing else. A mock
  that echoed a password would teach the frontend to read a field the device
  must never send.
"""

from __future__ import annotations

import secrets

from .auth import Auth, Session
from .clock import Clock
from .constants import (
    JSON_BODY_BYTES,
    LOG_PAGE_RECORDS,
    SCAN_RECORDS,
    SYSTEM_UPLOAD_MAX_BYTES,
    UPLOAD_CHUNK_BYTES,
    UPLOAD_MAX_BYTES,
)
from .firmware import Coprocessor, Firmware, Upload
from .jobs import JobStore
from .logs import Logs
from .matter import Matter
from .network import Network, Transaction
from .scenario import Scenario
from .system import System
from .wifi import WiFiScans


class DeviceState:
    """Everything above, sharing one clock, one boot id and one job store."""

    def __init__(self, clock: Clock | None = None, scenario: Scenario | None = None) -> None:
        self.clock = clock or Clock()
        self.scenario = scenario or Scenario()
        self.boot_id = f"boot_{secrets.token_hex(2)}"
        self.boot_ms = self.clock.now_ms()
        self.jobs = JobStore(self.clock, self.boot_id)
        self.auth = Auth(self.clock, self.scenario)
        self.network = Network(self.clock, self.jobs, self.scenario)
        self.wifi = WiFiScans(self.clock, self.jobs, self.scenario, self.network)
        self.matter = Matter(self.clock, self.jobs, self.scenario)
        self.firmware = Firmware(self.clock, self.jobs, self.scenario)
        self.coprocessor = Coprocessor(self.clock, self.jobs, self.scenario, self.firmware)
        self.system = System(
            self.clock, self.jobs, self.scenario, self.firmware, self.coprocessor, self.network
        )
        self.firmware.system = self.system
        self.logs = Logs(
            self.clock,
            self.boot_id,
            self.scenario,
            uart_mode=self.coprocessor.uart_mode,
            generation=lambda: self.coprocessor.generation,
        )
        self.idempotency: dict[str, tuple[str, dict[str, object]]] = {}

    def settle(self) -> None:
        """Called once at the top of every request, so time-driven transitions
        happen before anything reads state. Nothing here has a thread; the clock
        is the only thing that moves."""
        self.jobs.settle_all()
        self.network.settle()
        self.matter.settle()
        self.logs.settle()
        self.system.settle()
        if self.firmware.upload is not None:
            self.firmware._settle(self.firmware.upload)

    @property
    def uptime_ms(self) -> int:
        return self.clock.now_ms() - self.boot_ms

    def system_status_json(self) -> dict[str, object]:
        return {
            "device_id": "cedar-3i4o-0001",
            "model": "cedar_switch_3in4out_power_rev3",
            # The running image's MCUboot version, which an update changes.
            "firmware_version": self.system.running_version,
            "frontend_version": "0.0.0-mock",
            "boot_id": self.boot_id,
            "uptime_ms": str(self.uptime_ms),
            "wall_time": None,
            "active_job_ids": self.jobs.active_ids(),
        }

    def capabilities_json(self) -> dict[str, object]:
        logs_available, logs_reason = self.logs.esp32_availability()
        uart_available, uart_reason = self.coprocessor.uart_update_availability()
        system_available, system_reason = self.system.update_availability()
        return {
            "api_version": "1",
            "features": {
                "matter": {
                    "available": self.scenario.matter_state == "ready",
                    "reason": None
                    if self.scenario.matter_state == "ready"
                    else f"the stack is {self.scenario.matter_state}",
                },
                # Follows the UART's owner, as the device does: the C6's console
                # is readable whether or not ESP-Hosted ever comes up.
                "esp32_logs": {"available": logs_available, "reason": logs_reason},
                # The owner's decision, and the reason the UI must display
                # instead of a disabled placeholder control.
                "esp32_ota": {"available": False, "reason": Coprocessor.OTA_REASON},
                # Follows the UART's owner too: an install is how a coprocessor
                # with no firmware gets some, so its state does not gate it.
                "esp32_uart": {"available": uart_available, "reason": uart_reason},
                # The same answer as SystemFirmware.update.
                "stm32_update": {"available": system_available, "reason": system_reason},
            },
            "limits": {
                "json_body_bytes": JSON_BODY_BYTES,
                "upload_chunk_bytes": UPLOAD_CHUNK_BYTES,
                "upload_max_bytes": UPLOAD_MAX_BYTES,
                "system_upload_max_bytes": SYSTEM_UPLOAD_MAX_BYTES,
                "log_page_records": LOG_PAGE_RECORDS,
                "scan_records": SCAN_RECORDS,
                "commissioning_min_seconds": 180,
                "commissioning_max_seconds": 900,
                "network_confirm_min_seconds": 60,
                "network_confirm_max_seconds": 300,
            },
            "wifi_security_modes": ["open", "wpa2_psk", "wpa3_sae"],
            "firmware_formats": ["raw_full_flash", "mcuboot_image"],
            "update_requires_ethernet": True,
        }

    def to_json(self) -> dict[str, object]:
        """For `GET /__mock/state`: a debugging view, not part of the API."""
        return {
            "uptime_ms": self.uptime_ms,
            "boot_id": self.boot_id,
            "scenario": vars(self.scenario),
            "jobs": len(self.jobs),
            "revision": self.network.revision,
            "transaction": None
            if self.network.transaction is None
            else {
                "id": self.network.transaction.id,
                "state": self.network.transaction.state,
            },
            "upload": None if self.firmware.upload is None else self.firmware.upload.to_json(),
            "system": self.system.status_json(),
            "setup_required": self.auth.setup_required,
        }


__all__ = [
    "Auth",
    "Coprocessor",
    "DeviceState",
    "Firmware",
    "Logs",
    "Matter",
    "Network",
    "Scenario",
    "Session",
    "System",
    "Transaction",
    "Upload",
    "WiFiScans",
]
