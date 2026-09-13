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
    #: "offline" | "starting" | "ready" | "updating" | "recovering" | "failed"
    coprocessor_state: str = "ready"
    #: "ethernet" | "wifi". "wifi" makes an install request fail the contract's
    #: `ethernet_required` precondition, which the mock cannot observe for real.
    install_transport: str = "ethernet"
    storage_free_bytes: int = 8 * 1024 * 1024
    #: "ok" | "invalid_image" | "unsupported_target" | "incompatible_firmware"
    verify_result: str = "ok"
    #: "ok" | "recovery_required"
    install_result: str = "ok"
    #: Host names accepted besides IP literals and localhost, comma-separated;
    #: CONFIG_WEB_API_EXTRA_HOSTS on the device.
    extra_hosts: str = ""

    def update(self, values: dict[str, Any]) -> None:
        for key, value in values.items():
            if not hasattr(self, key):
                raise error("validation_failed", f"unknown scenario key {key!r}")
            setattr(self, key, value)
