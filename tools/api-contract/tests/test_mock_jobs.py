# SPDX-License-Identifier: Apache-2.0
"""The job, over time — the thing a mock that answers `succeeded` destroys.

A frontend's polling loop is only exercised by a job that is not finished when
first read. So these tests assert on the *sequence* a poller sees, not just on
the end state: that `queued` happens, that the phase changes, that progress
belongs to the phase and restarts at zero when the phase does, and that
`updated_uptime_ms` moves when something actually changed.
"""

from __future__ import annotations

from typing import Any

from cedar_contract.mock.clock import Clock, FrozenSource
from cedar_contract.mock.constants import (
    INSTALL_PHASE_MS,
    MATTER_OPEN_MS,
    QUEUE_MS,
    WIFI_SCAN_MS,
)
from cedar_contract.mock.jobs import CANCELLABLE_KINDS, Job, JobStore, Step
from cedar_contract.openapi import Document

from .conftest import Harness, build

SECOND = 1 / 1000


def _poll(harness: Harness, job_id: str, seconds: float, step: float = 0.25) -> list[Any]:
    """Poll like the UI does and keep the distinct states seen."""
    seen: list[Any] = []
    elapsed = 0.0
    while elapsed <= seconds:
        body = harness.client.get(f"/jobs/{job_id}").json
        entry = (body["state"], body["phase"])
        if not seen or seen[-1] != entry:
            seen.append(entry)
        harness.advance(step)
        elapsed += step
    return seen


def test_a_job_is_queued_before_it_runs(harness: Harness) -> None:
    accepted = harness.client.post("/network/wifi/scans", {})
    body = harness.client.get(f"/jobs/{accepted.json['job_id']}").json
    assert body["state"] == "queued"
    assert body["progress"] is None, "nothing has started, so there is nothing to count"


def test_a_job_moves_through_its_phases_rather_than_succeeding_at_once(harness: Harness) -> None:
    accepted = harness.client.post("/network/wifi/scans", {})
    seen = _poll(harness, accepted.json["job_id"], (QUEUE_MS + WIFI_SCAN_MS) * SECOND + 1)
    assert seen[0] == ("queued", "queued")
    assert ("running", "scanning") in seen
    assert seen[-1] == ("succeeded", "scanning")


def test_progress_belongs_to_the_phase_and_restarts_with_it(harness: Harness) -> None:
    """The contract says so explicitly, and warns against drawing it as one
    monotonic percentage. A mock with a single rising counter would invite
    exactly that."""
    source = FrozenSource()
    store = JobStore(Clock(source=source), "boot_test")
    job = store.create(
        "coprocessor_update",
        [
            Step("writing", 1000, "bytes", 4096),
            Step("verifying", 1000, "steps", 4),
        ],
    )
    source.value = 500
    first = store.get(job.id)
    assert first is not None and first.progress == {
        "completed": 2048,
        "total": 4096,
        "unit": "bytes",
    }
    source.value = 1000
    second = store.get(job.id)
    assert second is not None and second.progress == {"completed": 0, "total": 4, "unit": "steps"}
    assert second.phase == "verifying"


def test_updated_uptime_moves_only_when_something_changed(harness: Harness) -> None:
    accepted = harness.client.post(
        "/matter/commissioning", {"mode": "basic", "timeout_seconds": 300}
    )
    job_id = accepted.json["job_id"]
    first = harness.client.get(f"/jobs/{job_id}").json
    harness.advance(QUEUE_MS * SECOND / 3)
    second = harness.client.get(f"/jobs/{job_id}").json
    assert second["updated_uptime_ms"] == first["updated_uptime_ms"], "still queued"
    harness.advance((QUEUE_MS + MATTER_OPEN_MS) * SECOND)
    third = harness.client.get(f"/jobs/{job_id}").json
    assert int(third["updated_uptime_ms"]) > int(first["updated_uptime_ms"])
    assert third["state"] == "succeeded"


def test_uptime_fields_are_decimal_strings(harness: Harness) -> None:
    """The contract sends these as strings so JavaScript cannot lose precision on
    them, and the schema pattern is `^[0-9]+$`."""
    accepted = harness.client.post("/network/wifi/scans", {})
    body = harness.client.get(f"/jobs/{accepted.json['job_id']}").json
    for key in ("created_uptime_ms", "updated_uptime_ms"):
        assert isinstance(body[key], str) and body[key].isdigit()


def test_an_unknown_job_is_not_found(harness: Harness) -> None:
    response = harness.client.get("/jobs/job_deadbeef")
    assert response.status == 404
    assert response.json["error"]["code"] == "not_found"


def test_a_job_id_that_cannot_exist_is_also_not_found(harness: Harness) -> None:
    """The path parameter's own schema is enforced, so a malformed id is refused
    before it reaches a lookup — and as `not_found`, because that is what the
    contract says an unknown API URL is."""
    response = harness.client.get("/jobs/not%20an%20id")
    assert response.status == 404


def test_a_cancellable_job_can_be_cancelled(harness: Harness) -> None:
    accepted = harness.client.post("/network/wifi/scans", {})
    job_id = accepted.json["job_id"]
    assert harness.client.get(f"/jobs/{job_id}").json["cancellable"] is True
    assert harness.client.post(f"/jobs/{job_id}/cancel", {}).status == 202
    body = harness.client.get(f"/jobs/{job_id}").json
    assert body["state"] == "cancelled"
    assert body["cancellable"] is False, "there is nothing left to cancel"
    assert body["error"] is not None


def test_cancelling_a_finished_job_is_invalid_state(harness: Harness) -> None:
    accepted = harness.client.post("/network/wifi/scans", {})
    job_id = accepted.json["job_id"]
    harness.advance((QUEUE_MS + WIFI_SCAN_MS) * SECOND + 1)
    response = harness.client.post(f"/jobs/{job_id}/cancel", {})
    assert response.status == 409
    assert response.json["error"]["code"] == "invalid_state"


def test_a_network_apply_job_refuses_the_jobs_cancel_route(harness: Harness) -> None:
    """The contract routes this one through the transaction, so a cancellation
    cannot become a second network operation racing the first."""
    from .conftest import VALID_CANDIDATE

    revision = harness.client.get("/network/config").json["revision"]
    staged = harness.client.post(
        "/network/transactions", {"base_revision": revision, "config": VALID_CANDIDATE}
    )
    accepted = harness.client.post(
        f"/network/transactions/{staged.json['id']}/apply",
        {"confirmation_timeout_seconds": 120},
    )
    job = harness.client.get(f"/jobs/{accepted.json['job_id']}").json
    assert job["cancellable"] is False, (
        "and it says so in its own body: a UI that read `cancellable` would "
        "otherwise offer a Cancel button that always fails"
    )
    response = harness.client.post(f"/jobs/{accepted.json['job_id']}/cancel", {})
    assert response.status == 409
    assert "transaction" in response.json["error"]["message"]


def test_a_destructive_job_is_not_cancellable_at_all(harness: Harness) -> None:
    """The contract's own example carries `cancellable: false` on a coprocessor
    update, and says cancelling a destructive phase is impossible."""
    assert CANCELLABLE_KINDS["coprocessor_update"] is False
    assert CANCELLABLE_KINDS["upload_chunk"] is False
    assert CANCELLABLE_KINDS["network_apply"] is False, (
        "cancelled through the transaction's DELETE, so the jobs route must not "
        "advertise it either"
    )


def test_the_kinds_are_exactly_the_schema_enum(document: Document) -> None:
    """A kind the schema does not know would produce a response the frontend's
    generated types cannot hold."""
    declared = set(document.schemas["Job"]["properties"]["kind"]["enum"])
    assert set(CANCELLABLE_KINDS) == declared


def test_active_jobs_appear_in_the_system_status(harness: Harness) -> None:
    accepted = harness.client.post("/network/wifi/scans", {})
    job_id = accepted.json["job_id"]
    assert job_id in harness.client.get("/system/status").json["active_job_ids"]
    harness.advance((QUEUE_MS + WIFI_SCAN_MS) * SECOND + 1)
    assert job_id not in harness.client.get("/system/status").json["active_job_ids"]


def test_a_parked_job_reports_waiting_confirmation(harness: Harness) -> None:
    """Not a terminal state and not a phase: the job is waiting for the world."""
    from .conftest import VALID_CANDIDATE

    revision = harness.client.get("/network/config").json["revision"]
    staged = harness.client.post(
        "/network/transactions", {"base_revision": revision, "config": VALID_CANDIDATE}
    )
    accepted = harness.client.post(
        f"/network/transactions/{staged.json['id']}/apply",
        {"confirmation_timeout_seconds": 120},
    )
    harness.advance(5)
    body = harness.client.get(f"/jobs/{accepted.json['job_id']}").json
    assert body["state"] == "waiting_confirmation"
    harness.advance(60)
    assert harness.client.get(f"/jobs/{accepted.json['job_id']}").json["state"] == (
        "waiting_confirmation"
    ), "parking does not time out on its own; the transaction's timer does"


def test_resuming_a_job_that_is_not_parked_is_a_programming_error() -> None:
    store = JobStore(Clock(source=FrozenSource()), "boot_test")
    job = store.create("wifi_scan", [Step("scanning", 1000)])
    try:
        job.resume(0, [Step("more", 1)])
    except ValueError as exc:
        assert "not parked" in str(exc)
    else:
        raise AssertionError("a job that is not parked was resumed")


def test_finishing_a_job_with_a_non_terminal_state_is_refused() -> None:
    store = JobStore(Clock(source=FrozenSource()), "boot_test")
    job: Job = store.create("wifi_scan", [Step("scanning", 1000)])
    try:
        job.finish(0, "running")
    except ValueError as exc:
        assert "terminal" in str(exc)
    else:
        raise AssertionError("a job was finished into a running state")


def test_an_install_job_walks_the_first_versions_uart_phases(document: Document) -> None:
    """`activating` and `confirming` are absent on purpose: the contract says a
    successful `health_check` after a normal boot is the confirmation in the UART
    path, so a mock that emitted them would have the UI draw two steps the device
    never reaches."""
    from cedar_contract.mock.firmware import Coprocessor

    harness = build(document, setup_required=False)
    harness.client.login()
    upload = harness.client.post(
        "/firmware/uploads", {"filename": "c6.bin", "size_bytes": 256, "sha256": "0" * 64}
    ).json
    harness.client.put(f"/firmware/uploads/{upload['id']}/data?offset=0", raw=b"\x00" * 256)
    harness.advance(1)
    harness.client.post(f"/firmware/uploads/{upload['id']}/verify", {})
    harness.advance(5)
    accepted = harness.client.post(
        "/coprocessor/updates",
        {"upload_id": upload["id"], "method": "uart", "acknowledge_recovery": False},
    )
    seen = _poll(
        harness,
        accepted.json["job_id"],
        (QUEUE_MS + INSTALL_PHASE_MS * 12) * SECOND + 2,
        step=0.2,
    )
    phases = [phase for _state, phase in seen]
    assert phases[0] == "queued"
    ordered = [p for index, p in enumerate(phases) if index == 0 or phases[index - 1] != p]
    assert ordered[1:] == list(Coprocessor.INSTALL_PHASES)
    assert "activating" not in phases and "confirming" not in phases
    assert seen[-1] == ("succeeded", "complete")
