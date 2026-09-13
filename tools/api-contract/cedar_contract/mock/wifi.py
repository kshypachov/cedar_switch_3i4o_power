# SPDX-License-Identifier: Apache-2.0
"""Wi-Fi discovery: a `202` and a job, then results under the job id.

The AP list is chosen to be awkward on purpose. A hidden SSID has no display
name, an enterprise AP is visible but not connectable, one SSID appears on two
BSSIDs, and one security type is `unknown`. Every one of those is a case section
4's network screen has to render, and none of them appears if the fixture is
three tidy WPA2 rows.
"""

from __future__ import annotations

import base64
from typing import Any

from ..errors import error
from .clock import Clock
from .constants import QUEUE_MS, SCAN_RECORDS, WIFI_SCAN_MS
from .jobs import JobStore, Step
from .network import Network
from .scenario import Scenario
from .util import detail

_BASE_SCAN: tuple[dict[str, object], ...] = (
    {
        "ssid": "Cedar-Lab",
        "ssid_base64": "Q2VkYXItTGFi",
        "bssid": "a4:2b:b0:11:22:33",
        "channel": 6,
        "rssi_dbm": -41,
        "security": "wpa2_psk",
        "connect_supported": True,
    },
    {
        "ssid": "Cedar-Lab-5G",
        "ssid_base64": "Q2VkYXItTGFiLTVH",
        "bssid": "a4:2b:b0:11:22:34",
        "channel": 44,
        "rssi_dbm": -55,
        "security": "wpa3_sae",
        "connect_supported": True,
    },
    {
        "ssid": "Cedar-Lab",
        "ssid_base64": "Q2VkYXItTGFi",
        "bssid": "a4:2b:b0:99:88:77",
        "channel": 11,
        "rssi_dbm": -72,
        "security": "wpa2_psk",
        "connect_supported": True,
    },
    {
        "ssid": "guest",
        "ssid_base64": "Z3Vlc3Q=",
        "bssid": "de:ad:be:ef:00:01",
        "channel": 1,
        "rssi_dbm": -67,
        "security": "open",
        "connect_supported": True,
    },
    {
        "ssid": "office-wpa23",
        "ssid_base64": "b2ZmaWNlLXdwYTIz",
        "bssid": "de:ad:be:ef:00:02",
        "channel": 3,
        "rssi_dbm": -70,
        "security": "wpa2_wpa3_transition",
        "connect_supported": True,
    },
    {
        "ssid": "corp-eap",
        "ssid_base64": "Y29ycC1lYXA=",
        "bssid": "de:ad:be:ef:00:03",
        "channel": 9,
        "rssi_dbm": -63,
        "security": "enterprise",
        "connect_supported": False,
    },
    {
        "ssid": "",
        "ssid_base64": "",
        "bssid": "de:ad:be:ef:00:04",
        "channel": 13,
        "rssi_dbm": -78,
        "security": "wpa2_psk",
        "connect_supported": True,
    },
    {
        "ssid": "Кедр",
        "ssid_base64": "0JrQtdC00YA=",
        "bssid": "de:ad:be:ef:00:05",
        "channel": 7,
        "rssi_dbm": -59,
        "security": "wpa3_sae",
        "connect_supported": True,
    },
    {
        "ssid": "legacy-wep",
        "ssid_base64": "bGVnYWN5LXdlcA==",
        "bssid": "de:ad:be:ef:00:06",
        "channel": 2,
        "rssi_dbm": -81,
        "security": "unknown",
        "connect_supported": False,
    },
)


class WiFiScans:
    """`POST` a scan, poll the job, read the results by job id."""

    def __init__(self, clock: Clock, jobs: JobStore, scenario: Scenario, network: Network) -> None:
        self._clock = clock
        self._jobs = jobs
        self._scenario = scenario
        self._network = network
        self._scans: dict[str, dict[str, Any]] = {}
        self._latest: str | None = None

    def start(self) -> str:
        radio_error = self._network.wifi_radio_error()
        if radio_error is not None:
            raise error("capability_unavailable", f"{radio_error.message}; scanning is unavailable")
        if self._network.change_in_progress():
            # The contract forbids a scan during an apply: it takes the radio off
            # the channel of an interface whose new configuration is unconfirmed.
            raise error("busy", "A network change is being applied; scan after it finishes")
        for job_id in self._scans:
            job = self._jobs.get(job_id)
            if job is not None and not job.is_terminal:
                raise error("busy", "A Wi-Fi scan is already running")
        failed = self._scenario.wifi_scan == "failed"
        job = self._jobs.create(
            "wifi_scan",
            [
                Step("queued", QUEUE_MS, state="queued"),
                Step("scanning", WIFI_SCAN_MS, "steps", 11),
            ],
            resource_url=None,
            final_state="failed" if failed else "succeeded",
        )
        job.resource_url = f"/api/v1/network/wifi/scans/{job.id}"
        if failed:
            job.error = error(
                "service_not_ready", "The coprocessor did not answer the scan request"
            )
        self._scans[job.id] = {"mode": self._scenario.wifi_scan}
        self._latest = job.id
        return job.id

    def results_json(self, job_id: str) -> dict[str, object]:
        job = self._jobs.get(job_id)
        if job is None or job.kind != "wifi_scan":
            raise error("not_found", "No such Wi-Fi scan")
        if job.id != self._latest:
            # DEVICE RULE. The device keeps the results of one scan — 64 records
            # are kilobytes of RAM — so an older scan's job still exists but its
            # results are gone, which is the 410 row of the table.
            raise error("resource_expired", "A newer scan replaced these results")
        mode = self._scans[job.id]["mode"]
        done = job.state == "succeeded"
        items: list[dict[str, object]] = []
        truncated = False
        if done:
            if mode == "truncated":
                items = _fill_scan(SCAN_RECORDS)
                truncated = True
            else:
                items = [dict(ap) for ap in _BASE_SCAN]
        return {
            "job_id": job.id,
            "state": job.state,
            "items": items,
            "truncated": truncated,
            "error": None if job.error is None else detail(job.error),
        }


def _fill_scan(count: int) -> list[dict[str, object]]:
    """Exactly the record limit, so `truncated=true` is the honest answer.

    The contract caps a scan at 64 records and says `truncated` reports that
    more were seen. A frontend that never receives it renders a complete list
    where the device would have given a partial one.
    """
    items = [dict(ap) for ap in _BASE_SCAN]
    while len(items) < count:
        index = len(items)
        items.append(
            {
                "ssid": f"neighbour-{index:02d}",
                "ssid_base64": base64.b64encode(f"neighbour-{index:02d}".encode()).decode(),
                "bssid": f"02:00:5e:{index // 256:02x}:{index % 256:02x}:01",
                "channel": 1 + (index % 13),
                "rssi_dbm": -45 - (index % 45),
                "security": "wpa2_psk",
                "connect_supported": True,
            }
        )
    return items[:count]
