# SPDX-License-Identifier: Apache-2.0
"""Wi-Fi discovery, including the record limit the UI has to report.

The awkward cases are the point. A frontend built against three tidy WPA2 rows
has no code for a hidden SSID, an enterprise AP it must show but not offer, the
same SSID on two radios, or a security type it does not recognise — and every one
of those is in the contract.
"""

from __future__ import annotations

from cedar_contract.mock.constants import QUEUE_MS, SCAN_RECORDS, WIFI_SCAN_MS
from cedar_contract.openapi import Document

from .conftest import Harness, build

SECOND = 1 / 1000
SCANNED = (QUEUE_MS + WIFI_SCAN_MS) * SECOND + 0.1


def test_a_scan_is_accepted_as_a_job_and_read_back_by_job_id(harness: Harness) -> None:
    accepted = harness.client.post("/network/wifi/scans", {})
    assert accepted.status == 202
    job_id = accepted.json["job_id"]
    assert accepted.json["resource_url"] == f"/api/v1/network/wifi/scans/{job_id}"
    assert accepted.headers["Location"] == f"/api/v1/jobs/{job_id}"


def test_results_are_empty_while_the_scan_runs(harness: Harness) -> None:
    """A scan that answered with its results immediately would let the UI skip the
    spinner it needs for four seconds of radio time."""
    job_id = harness.client.post("/network/wifi/scans", {}).json["job_id"]
    running = harness.client.get(f"/network/wifi/scans/{job_id}").json
    assert running["state"] == "queued"
    assert running["items"] == []
    harness.advance(QUEUE_MS * SECOND + 0.1)
    assert harness.client.get(f"/network/wifi/scans/{job_id}").json["state"] == "running"
    harness.advance(SCANNED)
    assert harness.client.get(f"/network/wifi/scans/{job_id}").json["state"] == "succeeded"


def test_the_results_cover_the_cases_the_screen_has_to_render(harness: Harness) -> None:
    job_id = harness.client.post("/network/wifi/scans", {}).json["job_id"]
    harness.advance(SCANNED)
    items = harness.client.get(f"/network/wifi/scans/{job_id}").json["items"]

    assert any(ap["ssid"] == "" for ap in items), "a hidden SSID has no display name"
    enterprise = [ap for ap in items if ap["security"] == "enterprise"]
    assert enterprise and enterprise[0]["connect_supported"] is False, (
        "visible but not offered for connection, as the contract requires"
    )
    assert any(ap["security"] == "unknown" for ap in items)
    assert any(ap["security"] == "wpa2_wpa3_transition" for ap in items)

    by_ssid: dict[str, set[str]] = {}
    for ap in items:
        by_ssid.setdefault(ap["ssid"], set()).add(ap["bssid"])
    assert any(len(bssids) > 1 for bssids in by_ssid.values()), (
        "one SSID on two access points, which is why BSSID is kept"
    )


def test_ssid_base64_is_the_exact_bytes(harness: Harness) -> None:
    """`ssid` is for display and `ssid_base64` is the truth, which matters for an
    SSID that is not valid UTF-8 or that differs only in whitespace."""
    import base64

    job_id = harness.client.post("/network/wifi/scans", {}).json["job_id"]
    harness.advance(SCANNED)
    for ap in harness.client.get(f"/network/wifi/scans/{job_id}").json["items"]:
        decoded = base64.b64decode(ap["ssid_base64"], validate=True)
        assert len(decoded) <= 32
        assert decoded.decode("utf-8", "replace") == ap["ssid"]
    assert any(
        ap["ssid"] == "Кедр"
        for ap in harness.client.get(f"/network/wifi/scans/{job_id}").json["items"]
    ), "a non-ASCII SSID, because the UI must render it as text and not as HTML"


def test_a_full_scan_reports_truncated(document: Document) -> None:
    """The contract caps a scan at 64 records and uses `truncated` to say more
    were seen. A frontend that never receives it renders a complete list where
    the device would have given a partial one."""
    harness = build(document, setup_required=False, wifi_scan="truncated")
    harness.client.login()
    job_id = harness.client.post("/network/wifi/scans", {}).json["job_id"]
    harness.advance(SCANNED)
    results = harness.client.get(f"/network/wifi/scans/{job_id}").json
    assert results["truncated"] is True
    assert len(results["items"]) == SCAN_RECORDS


def test_an_incomplete_scan_is_not_reported_as_truncated(harness: Harness) -> None:
    job_id = harness.client.post("/network/wifi/scans", {}).json["job_id"]
    harness.advance(SCANNED)
    results = harness.client.get(f"/network/wifi/scans/{job_id}").json
    assert results["truncated"] is False
    assert 0 < len(results["items"]) < SCAN_RECORDS


def test_a_failed_scan_carries_the_error_in_the_results(document: Document) -> None:
    """After a `202` the failure is recorded in the job, and the results resource
    shows the same `ErrorDetail` — not a 500 on the poll."""
    harness = build(document, setup_required=False, wifi_scan="failed")
    harness.client.login()
    job_id = harness.client.post("/network/wifi/scans", {}).json["job_id"]
    harness.advance(SCANNED)
    results = harness.client.get(f"/network/wifi/scans/{job_id}").json
    assert results["state"] == "failed"
    assert results["items"] == []
    assert results["error"]["code"] == "service_not_ready"
    assert harness.client.get(f"/jobs/{job_id}").json["error"]["code"] == "service_not_ready"


def test_a_second_scan_while_one_runs_is_busy(harness: Harness) -> None:
    harness.client.post("/network/wifi/scans", {})
    second = harness.client.post("/network/wifi/scans", {})
    assert second.status == 409
    assert second.json["error"]["code"] == "busy"
    assert second.json["error"]["retryable"] is True


def test_a_scan_can_be_repeated_once_the_first_finished(harness: Harness) -> None:
    harness.client.post("/network/wifi/scans", {})
    harness.advance(SCANNED)
    assert harness.client.post("/network/wifi/scans", {}).status == 202


def test_an_unknown_scan_is_not_found(harness: Harness) -> None:
    assert harness.client.get("/network/wifi/scans/job_ffff").status == 404


def test_a_job_of_another_kind_is_not_a_scan(harness: Harness) -> None:
    """The results route is keyed by job id, so it has to refuse a job id that
    belongs to something else rather than answer with an empty scan."""
    accepted = harness.client.post(
        "/matter/commissioning", {"mode": "basic", "timeout_seconds": 300}
    )
    assert harness.client.get(f"/network/wifi/scans/{accepted.json['job_id']}").status == 404
