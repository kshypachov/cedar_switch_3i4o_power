# SPDX-License-Identifier: Apache-2.0
"""Every way the mock can be asked to behave differently.

Scenarios exist because the interesting screens are the unhappy ones, and none
of them is reachable by asking nicely: a scan that filled the record limit, an
image that fails verification, a coprocessor that is offline, an install request
that did not arrive over Ethernet. A mock that decided such things at random
would produce failures nobody can reproduce, so every one of them is a named
knob, set at startup or through `POST /__mock/scenario`.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from ..errors import error


@dataclass
class Scenario:
    """Every way the mock can be asked to behave differently.

    Set at startup (`--scenario key=value`) or at runtime through
    `POST /__mock/scenario`. Defaults describe a **fresh, unconfigured device**,
    because that is the first screen P2 has to build and the easiest one to
    forget: a mock that starts logged in lets the setup flow ship untested.
    """

    setup_required: bool = True
    admin_password: str = "cedar-mock-admin"
    setup_token: str = "cedar-mock-setup-token"
    #: "normal" | "truncated" | "failed"
    wifi_scan: str = "normal"
    #: "healthy" | "unhealthy". "unhealthy" refuses confirm with invalid_state,
    #: as the device does while a changed interface has no link, address or
    #: requested route yet.
    network_health: str = "healthy"
    #: Fabrics present at startup. A commissioning window that closes adds one.
    fabrics: int = 0
    #: "not_ready" | "starting" | "ready" | "failed"
    matter_state: str = "ready"
    #: "offline" | "starting" | "ready" | "updating" | "recovering" | "failed".
    #: It does not gate an install (P6): writing a whole image is how an offline
    #: or failed coprocessor gets firmware. A finished install sets it to "ready",
    #: or "failed" when the chip did not come back or a reboot cut the write short.
    coprocessor_state: str = "ready"
    #: "console" | "usb_bridge" | "flashing" | "unavailable": who owns the C6's
    #: UART. `esp32_logs` and `uart_update` follow it, not the ESP-Hosted
    #: transport (board B's C6 has no firmware and its ROM output is still
    #: logged). An install job reports "flashing" while it runs whatever this says.
    uart_mode: str = "console"
    #: The firmware version the C6 reports over ESP-Hosted while its transport
    #: is up (`CoprocessorStatus.firmware_version`); `host_protocol` is derived
    #: from its major. Not the image's `app_desc.version`, which is another
    #: number entirely ("1" for the current CP build).
    hosted_version: str = "v3.0.6"
    #: Log records appended per second of clock time; 0 keeps the rings still.
    log_rate_per_s: float = 0
    #: Records each ring (STM32, ESP32) keeps before it overwrites the oldest.
    log_ring_records: int = 10_000
    #: Records a page may look at; the device bounds each request's scan.
    log_scan_budget: int = 1_000_000
    #: Records of an export's snapshot overwritten while it was being sent,
    #: which the export marks with a `gap` record.
    log_export_lost: int = 0
    #: "ethernet" | "wifi". "wifi" makes an install request fail the contract's
    #: `ethernet_required` precondition, which the mock cannot observe for real.
    install_transport: str = "ethernet"
    storage_free_bytes: int = 8 * 1024 * 1024
    #: "ok" | "invalid_image" | "bare_app" | "unsupported_target" |
    #: "incompatible_firmware". "bare_app" is `invalid_image` with the message the
    #: device gives an application .bin sent instead of the merged file.
    verify_result: str = "ok"
    #: "ok" | "recovery_required"
    install_result: str = "ok"
    #: The STM32 image running at startup, `major.minor.revision+build`. Read when
    #: the device state is built (startup or reset); an update changes it after.
    system_version: str = "1.0.0+0"
    #: Whether that image is confirmed at startup. False starts the countdown.
    system_confirmed: bool = True
    #: How long a new image runs unconfirmed before it confirms itself.
    system_confirm_seconds: int = 1200
    #: Duration of each of the `preparing` and `requesting` phases.
    system_phase_ms: int = 1_500
    #: Duration of `rebooting`: the device answers the poll, then restarts.
    system_reboot_delay_ms: int = 2_000
    #: How long the device answers nothing while MCUboot swaps (the real device:
    #: ~40 s to the application). A connection gets no response at all.
    system_swap_ms: int = 5_000
    #: "ok" | "rejected" | "hangs". "rejected": MCUboot refuses slot 2 and the
    #: old firmware runs (`failed`). "hangs": the new image hangs, the watchdog
    #: resets it and MCUboot swaps back - twice the swap time (`failed`).
    #: `rolled_back` is a restart before confirmation: `POST /__mock/reboot`.
    system_swap_result: str = "ok"
    #: Host names accepted besides IP literals and localhost, comma-separated;
    #: CONFIG_WEB_API_EXTRA_HOSTS on the device.
    extra_hosts: str = ""

    def update(self, values: dict[str, Any]) -> None:
        for key, value in values.items():
            if not hasattr(self, key):
                raise error("validation_failed", f"unknown scenario key {key!r}")
            setattr(self, key, value)
