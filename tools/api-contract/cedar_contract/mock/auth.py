# SPDX-License-Identifier: Apache-2.0
"""Setup, login, the session and the CSRF token.

Cookie flags follow the contract's HTTP profile: `HttpOnly`, `SameSite=Strict`,
`Path=/`, and deliberately no `Secure`, because the device serves plain HTTP on
a trusted network and a `Secure` cookie would simply never be stored. The
frontend has to see the real flags to build against them.
"""

from __future__ import annotations

import secrets
from dataclasses import dataclass

from ..errors import error
from .clock import Clock
from .constants import SESSION_ABSOLUTE_MS, SESSION_IDLE_MS
from .scenario import Scenario


@dataclass
class Session:
    token: str
    csrf_token: str
    created_ms: int
    last_used_ms: int


class Auth:
    """Setup, login, the session, and the CSRF token that comes with it."""

    COOKIE = "cedar_session"

    def __init__(self, clock: Clock, scenario: Scenario) -> None:
        self._clock = clock
        self._scenario = scenario
        self.setup_required = scenario.setup_required
        self.setup_allowed = scenario.setup_required
        self.password = None if scenario.setup_required else scenario.admin_password
        self._sessions: dict[str, Session] = {}
        self._revoked: set[str] = set()

    # -- state -----------------------------------------------------------

    def state_json(self) -> dict[str, object]:
        # The token is published while setup is open (owner's decision, plan
        # section 13) and is null otherwise, exactly as the device answers.
        return {
            "setup_required": self.setup_required,
            "setup_allowed": self.setup_allowed,
            "setup_token": self._scenario.setup_token if self.setup_allowed else None,
        }

    def session_json(self, session: Session) -> dict[str, object]:
        age = self._clock.now_ms() - session.created_ms
        return {
            "username": "admin",
            "csrf_token": session.csrf_token,
            "idle_timeout_seconds": SESSION_IDLE_MS // 1000,
            "absolute_remaining_seconds": max(0, (SESSION_ABSOLUTE_MS - age) // 1000),
        }

    # -- transitions -----------------------------------------------------

    def setup(self, password: str, setup_token: str | None) -> Session:
        if not self.setup_allowed:
            # The contract closes the endpoint once an admin exists, and says so
            # with its own code rather than a generic 404.
            raise error("setup_not_allowed", "An administrator password is already set")
        if setup_token != self._scenario.setup_token:
            raise error("setup_not_allowed", "The setup token is not the one this device expects")
        self.password = password
        self.setup_required = False
        self.setup_allowed = False
        return self._issue()

    def login(self, password: str) -> Session:
        if self.password is None:
            raise error("setup_not_allowed", "This device has no administrator yet; run setup")
        if not secrets.compare_digest(password, self.password):
            raise error("invalid_credentials", "The password is not correct")
        return self._issue()

    def logout(self, session: Session) -> None:
        self._revoke(session.token)

    def change_password(self, current: str, new: str) -> None:
        """Checked before the job is accepted.

        DECISION. The contract returns `202` here and does not say where a wrong
        `current_password` surfaces. It is reported synchronously as
        `401 invalid_credentials`, the same code login uses, because the rule
        the contract does state — that errors after a `202` are the ones from
        hardware and SDKs — does not cover a credential the server can check
        before accepting any work. The frontend gets the answer on the request
        that carried the password rather than one poll later.
        """
        if self.password is not None and not secrets.compare_digest(current, self.password):
            raise error("invalid_credentials", "The current password is not correct")
        self._pending_password = new

    def commit_password(self) -> None:
        """Applied when the job completes; every session goes with it."""
        self.password = getattr(self, "_pending_password", self.password)
        for token in list(self._sessions):
            self._revoke(token)

    def resolve(self, token: str | None) -> Session:
        """The session behind a cookie, or the right refusal.

        The two 401s are not interchangeable. A cookie we never issued is
        `authentication_required` — there is nothing to expire. One that was
        valid and is not any more is `session_expired`, which the error table
        marks retryable precisely because logging in again is the fix.
        """
        if not token:
            raise error(
                "authentication_required",
                "This resource requires an administrator session",
            )
        if token in self._revoked:
            raise error("session_expired", "The session was revoked; sign in again")
        session = self._sessions.get(token)
        if session is None:
            raise error("authentication_required", "Unknown session")
        now = self._clock.now_ms()
        if now - session.last_used_ms > SESSION_IDLE_MS:
            self._revoke(token)
            raise error("session_expired", "The session was idle for too long; sign in again")
        if now - session.created_ms > SESSION_ABSOLUTE_MS:
            self._revoke(token)
            raise error("session_expired", "The session reached its maximum age; sign in again")
        session.last_used_ms = now
        return session

    def cookie(self, session: Session) -> str:
        return (
            f"{self.COOKIE}={session.token}; HttpOnly; SameSite=Strict; Path=/; "
            f"Max-Age={SESSION_ABSOLUTE_MS // 1000}"
        )

    def expiring_cookie(self) -> str:
        return f"{self.COOKIE}=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0"

    def _issue(self) -> Session:
        now = self._clock.now_ms()
        session = Session(
            token=secrets.token_urlsafe(24),
            csrf_token=secrets.token_urlsafe(32),
            created_ms=now,
            last_used_ms=now,
        )
        self._sessions[session.token] = session
        return session

    def _revoke(self, token: str) -> None:
        self._sessions.pop(token, None)
        self._revoked.add(token)
