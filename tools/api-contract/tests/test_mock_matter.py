# SPDX-License-Identifier: Apache-2.0
"""The commissioning window and what may be shown while it is open.

Two rules from the contract drive everything here. A window that is already open
is not silently replaced, because it may belong to another administrator. And
with no window open the codes resource answers `available=false` with a reason
and three nulls — never the factory QR, which would hand someone a passcode that
does not commission this device.
"""

from __future__ import annotations

import re

from cedar_contract.mock.constants import MATTER_CLOSE_MS, MATTER_OPEN_MS, QUEUE_MS
from cedar_contract.openapi import Document

from .conftest import Harness, build

SECOND = 1 / 1000
OPENED = (QUEUE_MS + MATTER_OPEN_MS) * SECOND + 0.1
CLOSED = MATTER_CLOSE_MS * SECOND + 0.1


def open_window(harness: Harness, timeout: int = 300) -> str:
    accepted = harness.client.post(
        "/matter/commissioning", {"mode": "basic", "timeout_seconds": timeout}
    )
    assert accepted.status == 202
    harness.advance(OPENED)
    return accepted.json["job_id"]


def test_a_fresh_device_is_not_commissioned(harness: Harness) -> None:
    status = harness.client.get("/matter/status").json
    assert status == {"state": "ready", "commissioned": False, "fabric_count": 0, "error": None}
    assert harness.client.get("/matter/fabrics").json == {"items": [], "count": 0}


def test_a_closed_window_offers_no_codes(harness: Harness) -> None:
    window = harness.client.get("/matter/commissioning").json
    assert window == {
        "open": False,
        "mode": None,
        "source": None,
        "remaining_seconds": 0,
        "codes_available": False,
    }
    codes = harness.client.get("/matter/onboarding-codes").json
    assert codes == {
        "available": False,
        "reason": "window_closed",
        "qr_payload": None,
        "manual_pairing_code": None,
        "setup_passcode": None,
    }


def test_opening_a_window_is_a_job_and_then_the_window_is_open(harness: Harness) -> None:
    job_id = open_window(harness)
    assert harness.client.get(f"/jobs/{job_id}").json["state"] == "succeeded"
    window = harness.client.get("/matter/commissioning").json
    assert window["open"] is True
    assert window["mode"] == "basic"
    assert window["source"] == "web"
    assert window["remaining_seconds"] == 300
    assert window["codes_available"] is True


def test_the_codes_are_three_different_values_in_the_declared_formats(harness: Harness) -> None:
    """`manual_pairing_code` and `setup_passcode` are different things, and the UI
    has to keep leading zeros on both, which is why they are strings."""
    open_window(harness)
    codes = harness.client.get("/matter/onboarding-codes").json
    assert codes["available"] is True and codes["reason"] is None
    assert codes["qr_payload"].startswith("MT:")
    assert re.fullmatch(r"[0-9]{11}|[0-9]{21}", codes["manual_pairing_code"])
    assert re.fullmatch(r"[0-9]{8}", codes["setup_passcode"])
    assert codes["manual_pairing_code"] != codes["setup_passcode"]


def test_the_window_counts_down_and_closes_itself(harness: Harness) -> None:
    open_window(harness, timeout=180)
    harness.advance(100)
    assert harness.client.get("/matter/commissioning").json["remaining_seconds"] == 80
    harness.advance(81)
    assert harness.client.get("/matter/commissioning").json["open"] is False
    assert harness.client.get("/matter/onboarding-codes").json["reason"] == "window_closed"


def test_opening_a_window_that_is_already_open_is_invalid_state(harness: Harness) -> None:
    """Not a silent replacement: the open window may be another administrator's."""
    open_window(harness)
    response = harness.client.post(
        "/matter/commissioning", {"mode": "basic", "timeout_seconds": 300}
    )
    assert response.status == 409
    assert response.json["error"]["code"] == "invalid_state"
    assert harness.client.get("/matter/commissioning").json["remaining_seconds"] == 300, (
        "the existing window is untouched"
    )


def test_the_same_idempotency_key_returns_the_original_job(harness: Harness) -> None:
    """The contract's distinction: a repeat of the same request is the same job,
    while a genuinely new request on an open window is the 409 above."""
    key = "matter-open-key-0001"
    first = harness.client.post(
        "/matter/commissioning", {"mode": "basic", "timeout_seconds": 300}, key=key
    )
    second = harness.client.post(
        "/matter/commissioning", {"mode": "basic", "timeout_seconds": 300}, key=key
    )
    assert second.status == 202
    assert second.json == first.json


def test_closing_an_already_closed_window_is_idempotent(harness: Harness) -> None:
    """The contract says so explicitly: still a `202`, and the job still succeeds."""
    first = harness.client.delete("/matter/commissioning")
    assert first.status == 202
    harness.advance(CLOSED)
    assert harness.client.get(f"/jobs/{first.json['job_id']}").json["state"] == "succeeded"
    second = harness.client.delete("/matter/commissioning")
    assert second.status == 202


def test_a_window_that_was_used_leaves_a_fabric(harness: Harness) -> None:
    """On the device a fabric appears when a controller commissions. The mock adds
    one when a window closes, because otherwise no frontend test ever reaches the
    populated fabric table — and an empty table after a successful commissioning
    is the one thing that screen must not show."""
    open_window(harness)
    harness.client.delete("/matter/commissioning")
    harness.advance(CLOSED)
    status = harness.client.get("/matter/status").json
    assert status["commissioned"] is True
    assert status["fabric_count"] == 1
    fabrics = harness.client.get("/matter/fabrics").json
    assert fabrics["count"] == 1 == len(fabrics["items"])


def test_a_second_fabric_is_added_without_removing_the_first(document: Document) -> None:
    """Multi-admin: the plan requires an additional fabric to join without
    evicting the previous one."""
    harness = build(document, setup_required=False, fabrics=2)
    harness.client.login()
    fabrics = harness.client.get("/matter/fabrics").json
    assert fabrics["count"] == 2
    first, second = fabrics["items"]
    assert first["id"] != second["id"]
    assert first["fabric_index"] != second["fabric_index"]
    assert first["fabric_id"] != second["fabric_id"]


def test_a_fabric_id_is_opaque_and_not_the_index(document: Document) -> None:
    """The schema says the identifier must include the root identity, because a
    fabric index may be reused after a fabric is removed."""
    harness = build(document, setup_required=False, fabrics=3)
    harness.client.login()
    for fabric in harness.client.get("/matter/fabrics").json["items"]:
        assert len(fabric["id"]) > 16
        assert str(fabric["fabric_index"]) != fabric["id"]
        assert re.fullmatch(r"[0-9A-Fa-f]{16}", fabric["fabric_id"])
        assert re.fullmatch(r"[0-9A-Fa-f]{16}", fabric["node_id"])
        assert 0 <= fabric["vendor_id"] <= 0xFFFF


def test_an_empty_label_is_a_real_answer(document: Document) -> None:
    """The contract warns that a label may be empty and that a vendor id does not
    name an ecosystem, so the UI must not guess. The fixture contains one."""
    harness = build(document, setup_required=False, fabrics=3)
    harness.client.login()
    labels = [f["label"] for f in harness.client.get("/matter/fabrics").json["items"]]
    assert "" in labels


def test_a_stack_that_is_not_ready_blocks_mutations_with_503(document: Document) -> None:
    harness = build(document, setup_required=False, matter_state="starting")
    harness.client.login()
    assert harness.client.get("/matter/status").json["state"] == "starting"
    response = harness.client.post(
        "/matter/commissioning", {"mode": "basic", "timeout_seconds": 300}
    )
    assert response.status == 503
    assert response.json["error"]["code"] == "service_not_ready"
    assert response.json["error"]["retryable"] is True


def test_a_failed_stack_reports_an_error_and_capabilities_agree(document: Document) -> None:
    harness = build(document, setup_required=False, matter_state="failed")
    harness.client.login()
    status = harness.client.get("/matter/status").json
    assert status["state"] == "failed"
    assert status["error"]["code"] == "service_not_ready"
    feature = harness.client.get("/capabilities").json["features"]["matter"]
    assert feature["available"] is False
    assert feature["reason"]


def test_a_timeout_outside_the_published_range_is_refused(harness: Harness) -> None:
    """The limits come from `capabilities`, and the schema carries the same
    bounds, so the rejection is a field error the form can place."""
    limits = harness.client.get("/capabilities").json["limits"]
    assert (limits["commissioning_min_seconds"], limits["commissioning_max_seconds"]) == (180, 900)
    for timeout in (
        limits["commissioning_min_seconds"] - 1,
        limits["commissioning_max_seconds"] + 1,
    ):
        response = harness.client.post(
            "/matter/commissioning", {"mode": "basic", "timeout_seconds": timeout}
        )
        assert response.status == 422
        assert response.json["error"]["fields"] == [
            {"path": "/timeout_seconds", "code": "out_of_range"}
        ]


def test_an_unknown_mode_is_refused(harness: Harness) -> None:
    """`enhanced` is a state the window can be *in* when a controller opened it,
    not a mode this API accepts — the contract says v1 opens `basic` only."""
    response = harness.client.post(
        "/matter/commissioning", {"mode": "enhanced", "timeout_seconds": 300}
    )
    assert response.status == 422
    assert response.json["error"]["fields"] == [{"path": "/mode", "code": "not_allowed"}]
