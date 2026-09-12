# SPDX-License-Identifier: Apache-2.0
"""The mock's own control plane, under `/__mock`.

It exists because two things a frontend has to handle are otherwise unreachable
from a test: a 120-second confirmation timeout, and the paths that only a
differently-behaving device produces. The namespace is the price — the device
serves nothing under `/__mock`, so a frontend that came to depend on it would
fail against the real thing immediately, which is the right way for that mistake
to surface.
"""

from __future__ import annotations

from cedar_contract.mock.wire import Request

from .conftest import Harness

MOCK = "/__mock"


def _control(harness: Harness, method: str, path: str, body: bytes = b"") -> object:
    return harness.app.handle(Request(method, f"{MOCK}/{path}", {}, body))


def test_the_control_plane_is_outside_the_api_base(harness: Harness) -> None:
    """Under `/__mock`, never under `/api/v1`, so no declared operation can be
    shadowed and nothing here can be mistaken for the contract."""
    assert not MOCK.startswith(harness.app.doc.base_path)
    assert harness.client.get("/__mock/state").status == 404, (
        "it is not reachable through the API base either"
    )


def test_state_is_readable_for_debugging(harness: Harness) -> None:
    body = _control(harness, "GET", "state").json  # type: ignore[attr-defined]
    assert set(body) >= {"uptime_ms", "boot_id", "scenario", "jobs", "revision"}


def test_advance_skips_time(harness: Harness) -> None:
    before = _control(harness, "GET", "state").json["uptime_ms"]  # type: ignore[attr-defined]
    _control(harness, "POST", "advance", b'{"seconds": 130}')
    after = _control(harness, "GET", "state").json["uptime_ms"]  # type: ignore[attr-defined]
    assert after - before == 130_000


def test_advance_reaches_the_confirmation_timeout(harness: Harness) -> None:
    """The reason the endpoint exists: an e2e run can see the rollback without
    waiting two minutes for it."""
    from .conftest import VALID_CANDIDATE

    revision = harness.client.get("/network/config").json["revision"]
    staged = harness.client.post(
        "/network/transactions", {"base_revision": revision, "config": VALID_CANDIDATE}
    )
    transaction = staged.json["id"]
    harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    _control(harness, "POST", "advance", b'{"seconds": 10}')
    assert (
        harness.client.get(f"/network/transactions/{transaction}").json["state"]
        == "awaiting_confirmation"
    )
    _control(harness, "POST", "advance", b'{"seconds": 125}')
    assert harness.client.get(f"/network/transactions/{transaction}").json["state"] in (
        "rolling_back",
        "rolled_back",
    )


def test_time_never_goes_backwards(harness: Harness) -> None:
    """A timer that can run backwards is a state machine with no invariants left."""
    response = _control(harness, "POST", "advance", b'{"seconds": -5}')
    assert response.status == 422  # type: ignore[attr-defined]


def test_reset_returns_the_device_to_a_fresh_state(harness: Harness) -> None:
    harness.client.post("/network/wifi/scans", {})
    assert len(harness.state.jobs) == 1
    _control(harness, "POST", "reset")
    assert len(harness.app.state.jobs) == 0
    assert harness.app.state.auth.setup_required is True
    assert harness.app.handle(Request("GET", "/api/v1/system/status")).status == 401, (
        "the session went with it"
    )


def test_reset_can_choose_a_scenario(harness: Harness) -> None:
    _control(
        harness,
        "POST",
        "reset",
        b'{"scenario": {"setup_required": false, "wifi_scan": "truncated", "fabrics": 2}}',
    )
    assert harness.app.state.scenario.wifi_scan == "truncated"
    assert harness.app.state.auth.setup_required is False
    assert harness.app.state.matter.fabrics_json()["count"] == 2


def test_a_scenario_can_be_changed_without_a_reset(harness: Harness) -> None:
    """So that a long e2e run can turn one thing awkward mid-flight rather than
    starting over and logging in again."""
    _control(harness, "POST", "scenario", b'{"coprocessor_state": "offline"}')
    assert harness.client.get("/coprocessor/status").json["state"] == "offline"
    assert harness.client.get("/capabilities").json["features"]["esp32_uart"]["available"] is False


def test_an_unknown_scenario_key_is_refused(harness: Harness) -> None:
    """A typo that silently did nothing would leave a test asserting on a device
    that never changed."""
    response = _control(harness, "POST", "scenario", b'{"wifi_scanning": "truncated"}')
    assert response.status == 422  # type: ignore[attr-defined]
    assert "wifi_scanning" in response.json["error"]["message"]  # type: ignore[attr-defined]


def test_an_unknown_control_endpoint_is_not_found(harness: Harness) -> None:
    assert _control(harness, "POST", "reboot").status == 404  # type: ignore[attr-defined]
