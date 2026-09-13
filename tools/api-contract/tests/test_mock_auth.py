# SPDX-License-Identifier: Apache-2.0
"""Setup, login, the session, and the password change that ends every session.

The state worth having here is that the device starts unconfigured. `/auth/state`
answering `setup_required: true` is the first thing P2's shell has to branch on,
and a mock that came up already signed in would let that branch ship untested.
"""

from __future__ import annotations

from cedar_contract.mock.auth import Auth
from cedar_contract.mock.constants import (
    PASSWORD_CHANGE_MS,
    SESSION_ABSOLUTE_MS,
    SESSION_IDLE_MS,
)

from .conftest import PASSWORD, SETUP_TOKEN, Harness

SECOND = 1 / 1000


def test_a_fresh_device_asks_for_setup(fresh: Harness) -> None:
    state = fresh.client.get("/auth/state", authenticate=False).json
    assert state == {
        "setup_required": True,
        "setup_allowed": True,
        "setup_token": SETUP_TOKEN,
    }


def test_login_before_setup_is_refused_with_its_own_code(fresh: Harness) -> None:
    """Not `invalid_credentials`: there is no credential to be wrong about, and
    telling the client to try another password would be a false lead."""
    response = fresh.client.login()
    assert response.status == 403
    assert response.json["error"]["code"] == "setup_not_allowed"


def test_setup_needs_the_setup_token(fresh: Harness) -> None:
    assert fresh.client.setup(token="wrong-token-but-long").status == 403
    assert fresh.client.get("/auth/state", authenticate=False).json["setup_required"] is True


def test_setup_without_the_token_header_is_refused(fresh: Harness) -> None:
    response = fresh.client.post("/auth/setup", {"password": PASSWORD}, authenticate=False)
    assert response.status == 403
    assert response.json["error"]["code"] == "setup_not_allowed"


def test_setup_issues_a_session_and_closes_itself(fresh: Harness) -> None:
    """The contract closes the endpoint once an administrator exists, so a second
    setup cannot silently take the device over."""
    response = fresh.client.setup()
    assert response.status == 201
    assert response.headers["Location"] == "/api/v1/auth/session"
    assert fresh.client.get("/auth/state", authenticate=False).json == {
        "setup_required": False,
        "setup_allowed": False,
        "setup_token": None,
    }
    assert fresh.client.setup().status == 403


def test_the_session_cookie_carries_the_http_profile_flags(fresh: Harness) -> None:
    """No `Secure`, deliberately: the device serves plain HTTP on a trusted
    network, and a `Secure` cookie would never be stored at all."""
    cookie = fresh.client.setup().headers["Set-Cookie"]
    assert cookie.startswith(f"{Auth.COOKIE}=")
    assert "HttpOnly" in cookie
    assert "SameSite=Strict" in cookie
    assert "Path=/" in cookie
    assert "Secure" not in cookie


def test_a_wrong_password_is_invalid_credentials(harness: Harness) -> None:
    response = harness.client.login("not-the-password")
    assert response.status == 401
    assert response.json["error"]["code"] == "invalid_credentials"
    assert response.json["error"]["retryable"] is False


def test_a_password_is_never_reflected_back(harness: Harness) -> None:
    """Not in the session, not in the rejection. The contract is explicit, and a
    mock that echoed one would teach the frontend to read a field that must not
    exist."""
    body = harness.client.get("/auth/session").body.decode()
    assert PASSWORD not in body
    rejected = harness.client.login("a-distinctive-wrong-password").body.decode()
    assert "a-distinctive-wrong-password" not in rejected


def test_an_unknown_cookie_is_authentication_required(harness: Harness) -> None:
    """There is nothing to expire, so `session_expired` would be a lie — and it
    is the retryable one, which would send the client into a re-login loop it
    cannot win."""
    response = harness.client.get(
        "/system/status", headers={"cookie": "cedar_session=never-issued"}
    )
    assert response.status == 401
    assert response.json["error"]["code"] == "authentication_required"


def test_logout_revokes_the_cookie_and_the_session(harness: Harness) -> None:
    response = harness.client.delete("/auth/session")
    assert response.status == 204
    assert response.body == b""
    assert "Max-Age=0" in response.headers["Set-Cookie"]
    after = harness.client.get("/system/status")
    assert after.status == 401
    assert after.json["error"]["code"] == "session_expired", "it was valid and now is not"


def test_an_idle_session_expires(harness: Harness) -> None:
    harness.advance(SESSION_IDLE_MS * SECOND - 1)
    assert harness.client.get("/system/status").status == 200
    harness.advance(SESSION_IDLE_MS * SECOND + 1)
    response = harness.client.get("/system/status")
    assert response.status == 401
    assert response.json["error"]["code"] == "session_expired"
    assert response.json["error"]["retryable"] is True, "logging in again is the fix"


def test_activity_postpones_the_idle_timeout_but_not_the_absolute_one(harness: Harness) -> None:
    """The two lifetimes mean different things, and a mock with only one would let
    the frontend treat a working session as permanent."""
    for _ in range(10):
        harness.advance(SESSION_IDLE_MS * SECOND / 2)
        assert harness.client.get("/system/status").status == 200
    remaining = harness.client.get("/auth/session").json["absolute_remaining_seconds"]
    assert 0 < remaining < SESSION_ABSOLUTE_MS // 1000
    harness.advance(SESSION_ABSOLUTE_MS * SECOND)
    assert harness.client.get("/system/status").status == 401


def test_a_password_change_is_a_job_and_revokes_every_session(harness: Harness) -> None:
    other = harness.client.__class__(app=harness.app, document=harness.client.document)
    assert other.login().status == 200

    response = harness.client.put(
        "/auth/password",
        {"current_password": PASSWORD, "new_password": "a-much-longer-secret"},
    )
    assert response.status == 202
    job_url = response.headers["Location"]
    assert job_url.endswith(response.json["job_id"])
    assert response.headers["Retry-After"]

    # Still valid while the change is in flight: the contract revokes on success.
    assert harness.client.get("/system/status").status == 200
    harness.advance(PASSWORD_CHANGE_MS * SECOND + 0.1)

    assert harness.client.get("/system/status").json["error"]["code"] == "session_expired"
    assert other.get("/system/status").status == 401, "every session, not just the caller's"
    assert harness.client.login(PASSWORD).status == 401
    assert harness.client.login("a-much-longer-secret").status == 200


def test_a_wrong_current_password_is_refused_before_the_job(harness: Harness) -> None:
    """DECISION, recorded in `auth.py` and the README: reported synchronously as
    `401 invalid_credentials` rather than as a failed job, because the rule about
    errors after a `202` covers hardware and SDK failures, not a credential the
    server can check before accepting any work."""
    response = harness.client.put(
        "/auth/password",
        {"current_password": "wrong", "new_password": "a-much-longer-secret"},
    )
    assert response.status == 401
    assert response.json["error"]["code"] == "invalid_credentials"
    assert harness.client.login(PASSWORD).status == 200, "the old password still works"


def test_a_short_new_password_is_a_field_error_the_frontend_can_place(harness: Harness) -> None:
    """`out_of_range`, from the closed set: there is no `too_short` code, and the
    honest reading of a 12-character minimum is a length out of range."""
    response = harness.client.put(
        "/auth/password", {"current_password": PASSWORD, "new_password": "short"}
    )
    assert response.status == 422
    assert response.json["error"]["fields"] == [{"path": "/new_password", "code": "out_of_range"}]


def test_setup_token_is_the_scenario_value(fresh: Harness) -> None:
    assert fresh.state.scenario.setup_token == SETUP_TOKEN


def test_the_setup_token_is_published_only_while_setup_is_open(fresh: Harness) -> None:
    """Owner's decision: the token is shown in the web interface. It is what the
    page reads to fill X-Setup-Token, and it disappears with setup."""
    state = fresh.client.get("/auth/state", authenticate=False).json
    assert fresh.client.setup(token=state["setup_token"]).status == 201
    assert fresh.client.get("/auth/state", authenticate=False).json["setup_token"] is None
