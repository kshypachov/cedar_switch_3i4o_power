# SPDX-License-Identifier: Apache-2.0
"""Timings, limits and the factory configuration, in one place.

Timings are named rather than written into each state machine, for two reasons.
A test asserts on the constant, so a scan that gets slower cannot silently turn
a passing test into one that measures nothing. And they are all chosen against
one requirement from the contract: a UI polling every 500-1000 ms must observe
every transition, so no phase is shorter than that.

The limits are the values `GET /capabilities` publishes, and the same values the
mock enforces. The contract lets a server publish smaller limits than the design
maxima, and a mock that advertised one number and enforced another would be the
exact bug the capabilities resource exists to prevent.
"""

from __future__ import annotations

from typing import Any

# polling at the contract's 500-1000 ms actually observes every transition.

QUEUE_MS = 300
MATTER_OPEN_MS = 1_200
MATTER_CLOSE_MS = 800
NETWORK_APPLY_MS = 3_000
NETWORK_COMMIT_MS = 1_500
NETWORK_ROLLBACK_MS = 2_000
NETWORK_DISCARD_MS = 400
WIFI_SCAN_MS = 4_000
UPLOAD_CHUNK_MS = 200
FIRMWARE_VERIFY_MS = 2_500
FIRMWARE_DELETE_MS = 600
PASSWORD_CHANGE_MS = 1_200
INSTALL_PHASE_MS = 1_500

#: A staged candidate lives 300 seconds before apply, per the contract.
CANDIDATE_TTL_MS = 300_000
#: Session lifetimes: the contract's starting values.
SESSION_IDLE_MS = 30 * 60 * 1_000
SESSION_ABSOLUTE_MS = 8 * 60 * 60 * 1_000

#: The device's published limits, echoed in capabilities and enforced here.
JSON_BODY_BYTES = 8_192
UPLOAD_CHUNK_BYTES = 16_384
UPLOAD_MAX_BYTES = 2_097_152
LOG_PAGE_RECORDS = 100
#: The device builds a log page in its response buffer
#: (CONFIG_WEB_API_RESPONSE_BODY_MAX); a page stops before the record that
#: would not fit.
LOG_PAGE_BYTES = 16_384
#: Room the device keeps for the cursor while it fills that buffer.
LOG_CURSOR_RESERVE = 64
SCAN_RECORDS = 64


#: What a device ships with: Ethernet on DHCP, Wi-Fi unconfigured.
DEFAULT_CONFIG: dict[str, Any] = {
    "preferred_interface": "ethernet",
    "dns": {"mode": "automatic", "servers": []},
    "interfaces": {
        "ethernet": {
            "enabled": True,
            "ipv4": {"mode": "dhcp", "address": None, "prefix_length": None, "gateway": None},
        },
        "wifi": {
            "enabled": False,
            "ssid_base64": "",
            "security": "open",
            "hidden": False,
            "ipv4": {"mode": "dhcp", "address": None, "prefix_length": None, "gateway": None},
            "password_set": False,
        },
    },
}
