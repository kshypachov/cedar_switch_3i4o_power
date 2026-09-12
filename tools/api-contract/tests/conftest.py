# SPDX-License-Identifier: Apache-2.0
"""Fixtures for the mock's tests, which are also the contract test.

The important one is `Client.request()`: it validates every response against the
schema the document declares for the operation it reached, for success and for
rejection alike. That is deliberate. If only one test did the schema check, every
other test would be free to assert on a body that the device could never send;
putting the check in the client means a test cannot pass against an invalid
response even by accident, and the contract test is then not a separate exercise
but the floor under all of them.
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from cedar_contract.mock.app import MockApp  # noqa: E402
from cedar_contract.mock.clock import Clock, FrozenSource  # noqa: E402
from cedar_contract.mock.scenario import Scenario  # noqa: E402
from cedar_contract.mock.wire import Request, Response  # noqa: E402
from cedar_contract.openapi import Document  # noqa: E402

BASE = "/api/v1"
#: The mock's own credentials. Not secrets — they are printed at startup.
PASSWORD = "cedar-mock-admin"
SETUP_TOKEN = "cedar-mock-setup-token"


@pytest.fixture(scope="session")
def document() -> Document:
    """Loaded once: building the validators for 48 schemas is not free."""
    return Document.load()


@dataclass
class Client:
    """A caller that keeps the session, and checks every answer.

    `key` is generated per call rather than fixed, because the contract makes a
    repeated `Idempotency-Key` mean "the same action" — a client that reused one
    would deduplicate requests the test meant to be separate. Tests that are
    about idempotency pass the key explicitly.
    """

    app: MockApp
    document: Document
    cookie: str | None = None
    csrf: str | None = None
    _keys: int = 0
    #: Every (operation_id, status) this client has seen, for the coverage test.
    seen: set[tuple[str, int]] = field(default_factory=set)

    # -- calling ---------------------------------------------------------

    def request(
        self,
        method: str,
        path: str,
        body: Any = None,
        *,
        raw: bytes | None = None,
        key: str | None = None,
        headers: dict[str, str] | None = None,
        authenticate: bool = True,
    ) -> Response:
        sent: dict[str, str] = {}
        if authenticate and self.cookie:
            sent["cookie"] = f"cedar_session={self.cookie}"
        if authenticate and self.csrf:
            sent["x-csrf-token"] = self.csrf
        data = b""
        if body is not None:
            data = json.dumps(body).encode()
            sent["content-type"] = "application/json"
        elif raw is not None:
            data = raw
            sent["content-type"] = "application/octet-stream"
        if method != "GET":
            sent["idempotency-key"] = key or self.next_key()
        sent.update(headers or {})
        response = self.app.handle(Request(method, f"{BASE}{path}", sent, data))
        self._check(method, path, response)
        return response

    def get(self, path: str, **kw: Any) -> Response:
        return self.request("GET", path, **kw)

    def post(self, path: str, body: Any = None, **kw: Any) -> Response:
        return self.request("POST", path, body, **kw)

    def put(self, path: str, body: Any = None, **kw: Any) -> Response:
        return self.request("PUT", path, body, **kw)

    def delete(self, path: str, **kw: Any) -> Response:
        return self.request("DELETE", path, **kw)

    def next_key(self) -> str:
        self._keys += 1
        return f"test-key-{self._keys:012d}"

    # -- session ---------------------------------------------------------

    def login(self, password: str = PASSWORD) -> Response:
        response = self.post("/auth/session", {"password": password}, authenticate=False)
        if response.status == 200:
            self._adopt(response)
        return response

    def setup(self, password: str = PASSWORD, token: str = SETUP_TOKEN) -> Response:
        response = self.post(
            "/auth/setup",
            {"password": password},
            headers={"x-setup-token": token},
            authenticate=False,
        )
        if response.status == 201:
            self._adopt(response)
        return response

    def _adopt(self, response: Response) -> None:
        cookie = response.headers.get("Set-Cookie", "")
        self.cookie = cookie.split("=", 1)[1].split(";")[0]
        self.csrf = response.json["csrf_token"]

    # -- the check that makes this the contract test ----------------------

    def _check(self, method: str, path: str, response: Response) -> None:
        operation = self.app.match(method, f"{BASE}{path}")
        if operation is None:
            return
        self.seen.add((operation.operation_id, response.status))
        assert response.headers.get("X-Request-ID"), "every response carries X-Request-ID"
        assert response.headers.get("Cache-Control") == "no-store"
        if response.status >= 400:
            schema: Any = {"$ref": "#/components/schemas/Error"}
            # The body's request_id is the same value as the header. The contract
            # sends `X-Request-ID` so a client can correlate a complaint with a
            # device log line, and two different ids on one rejection make that
            # impossible in exactly the case it matters.
            assert response.json["error"]["request_id"] == response.headers["X-Request-ID"]
        elif response.status == 204:
            assert response.body == b"", "204 carries no body"
            return
        else:
            expected = int(operation.success_status)
            assert response.status == expected, (
                f"{operation.operation_id} answered {response.status}, "
                f"and the document declares only {expected} and default"
            )
            schema = operation.success_schema()
            if schema is None:
                return  # a non-JSON success, checked by its own test
        errors = list(self.document.validator(schema).iter_errors(response.json))
        assert not errors, (
            f"{method} {path} -> {response.status} does not match its schema: "
            + "; ".join(f"{list(e.absolute_path)}: {e.message}" for e in errors)
        )


@dataclass
class Harness:
    """The app, a clock that only moves when told, and a caller."""

    app: MockApp
    client: Client
    source: FrozenSource

    def advance(self, seconds: float) -> None:
        """Skip forward. Tests use seconds because the contract does."""
        self.source.value += int(seconds * 1000)
        self.app.state.settle()

    @property
    def state(self) -> Any:
        return self.app.state


def build(document: Document, **scenario: Any) -> Harness:
    source = FrozenSource()
    app = MockApp(document=document, clock=Clock(source=source), scenario=Scenario(**scenario))
    return Harness(app=app, client=Client(app=app, document=document), source=source)


@pytest.fixture
def fresh(document: Document) -> Harness:
    """An unconfigured device: `setup_required` is true and nobody is signed in."""
    return build(document)


@pytest.fixture
def harness(document: Document) -> Harness:
    """A configured device with an administrator signed in.

    Almost every test wants this, and having it here keeps the two lines of
    login out of forty test bodies.
    """
    built = build(document, setup_required=False)
    assert built.client.login().status == 200
    return built


#: A candidate configuration that passes every business rule: static Ethernet in
#: the lab subnet with a gateway inside it, manual DNS, Wi-Fi off.
VALID_CANDIDATE: dict[str, Any] = {
    "preferred_interface": "ethernet",
    "dns": {"mode": "manual", "servers": ["192.168.88.1"]},
    "interfaces": {
        "ethernet": {
            "enabled": True,
            "ipv4": {
                "mode": "static",
                "address": "192.168.88.50",
                "prefix_length": 24,
                "gateway": "192.168.88.1",
            },
        },
        "wifi": {
            "enabled": False,
            "ssid_base64": "",
            "security": "open",
            "hidden": False,
            "ipv4": {"mode": "dhcp", "address": None, "prefix_length": None, "gateway": None},
            "credential": {"action": "keep"},
        },
    },
}


def candidate(**changes: Any) -> dict[str, Any]:
    """A copy of `VALID_CANDIDATE` with nested keys replaced.

    Paths are slash-separated, so a test says what it is breaking in one line:
    `candidate(**{"interfaces/ethernet/ipv4/gateway": "10.0.0.1"})`.
    """
    config = json.loads(json.dumps(VALID_CANDIDATE))
    for path, value in changes.items():
        node = config
        parts = path.split("/")
        for part in parts[:-1]:
            node = node[part]
        node[parts[-1]] = value
    return config
