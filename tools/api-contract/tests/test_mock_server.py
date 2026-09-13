# SPDX-License-Identifier: Apache-2.0
"""The HTTP adapter, over a real socket.

Everything else in this suite calls `MockApp.handle()` directly, which is the
right place to test behaviour. But the mock's reason to exist is that a browser
can talk to it, and none of those tests would notice a `Content-Length` that is
wrong, a cookie that never reaches the wire, or a 204 that sends a body anyway.
So this module starts the real server on an ephemeral port and uses `urllib`.

Small on purpose: it checks the transport, not the contract.
"""

from __future__ import annotations

import json
import threading
import urllib.error
import urllib.request
from collections.abc import Iterator
from http.server import HTTPServer

import pytest
from cedar_contract.mock.app import MockApp
from cedar_contract.mock.scenario import Scenario
from cedar_contract.mock.server import _Handler
from cedar_contract.openapi import Document


@pytest.fixture
def server(document: Document) -> Iterator[str]:
    app = MockApp(document=document, scenario=Scenario(setup_required=False))
    handler = type("_BoundHandler", (_Handler,), {"app": app, "lock": threading.Lock()})
    httpd = HTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{httpd.server_address[1]}"
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join(timeout=5)


def _call(
    url: str, method: str = "GET", body: bytes | None = None, headers: dict[str, str] | None = None
) -> tuple[int, dict[str, str], bytes]:
    request = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def test_a_get_is_served_with_a_correct_content_length(server: str) -> None:
    status, headers, body = _call(f"{server}/api/v1/auth/state")
    assert status == 200
    assert headers["Content-Type"] == "application/json"
    assert int(headers["Content-Length"]) == len(body)
    assert json.loads(body) == {
        "setup_required": False,
        "setup_allowed": False,
        "setup_token": None,
    }


def test_the_contract_headers_survive_the_transport(server: str) -> None:
    _status, headers, _body = _call(f"{server}/api/v1/auth/state")
    assert headers["Cache-Control"] == "no-store"
    assert headers["X-Request-ID"].startswith("req_")


def test_a_session_cookie_round_trips(server: str) -> None:
    status, headers, body = _call(
        f"{server}/api/v1/auth/session",
        "POST",
        b'{"password": "cedar-mock-admin"}',
        {"Content-Type": "application/json"},
    )
    assert status == 200
    cookie = headers["Set-Cookie"]
    assert "HttpOnly" in cookie and "Secure" not in cookie
    session = cookie.split(";")[0]

    status, _headers, protected = _call(
        f"{server}/api/v1/system/status", headers={"Cookie": session}
    )
    assert status == 200
    assert json.loads(protected)["boot_id"]

    status, _headers, _body = _call(f"{server}/api/v1/system/status")
    assert status == 401, "and without the cookie it is refused"
    _ = json.loads(body)


def test_a_rejection_arrives_as_json_with_its_status(server: str) -> None:
    status, headers, body = _call(f"{server}/api/v1/nothing")
    assert status == 404
    assert headers["Content-Type"] == "application/json"
    assert json.loads(body)["error"]["code"] == "not_found"


def test_a_204_sends_no_body(server: str) -> None:
    _status, headers, body = _call(
        f"{server}/api/v1/auth/session",
        "POST",
        b'{"password": "cedar-mock-admin"}',
        {"Content-Type": "application/json"},
    )
    session = headers["Set-Cookie"].split(";")[0]
    csrf = json.loads(body)["csrf_token"]
    status, headers, body = _call(
        f"{server}/api/v1/auth/session",
        "DELETE",
        headers={"Cookie": session, "X-CSRF-Token": csrf},
    )
    assert status == 204
    assert body == b""
    assert headers["Content-Length"] == "0"


def test_binary_upload_data_reaches_the_handler_intact(server: str) -> None:
    _status, headers, body = _call(
        f"{server}/api/v1/auth/session",
        "POST",
        b'{"password": "cedar-mock-admin"}',
        {"Content-Type": "application/json"},
    )
    auth = {
        "Cookie": headers["Set-Cookie"].split(";")[0],
        "X-CSRF-Token": json.loads(body)["csrf_token"],
        "Idempotency-Key": "server-test-key-0001",
    }
    status, _headers, created = _call(
        f"{server}/api/v1/firmware/uploads",
        "POST",
        json.dumps({"filename": "c6.bin", "size_bytes": 256, "sha256": "0" * 64}).encode(),
        {**auth, "Content-Type": "application/json"},
    )
    assert status == 201
    upload_id = json.loads(created)["id"]
    payload = bytes(range(256))
    status, _headers, _body = _call(
        f"{server}/api/v1/firmware/uploads/{upload_id}/data?offset=0",
        "PUT",
        payload,
        {
            **auth,
            "Content-Type": "application/octet-stream",
            "Idempotency-Key": "chunk-key-000001",
        },
    )
    assert status == 202


def test_the_control_plane_is_reachable_over_http(server: str) -> None:
    status, _headers, body = _call(f"{server}/__mock/state")
    assert status == 200
    assert json.loads(body)["boot_id"]


def test_options_answers_without_granting_cross_origin_access(server: str) -> None:
    """Same origin in production, so CORS is not modelled. An `OPTIONS` that
    allowed cross-origin requests would let the frontend be developed against a
    permission the device does not grant."""
    status, headers, _body = _call(f"{server}/api/v1/auth/state", "OPTIONS")
    assert status == 204
    assert "Allow" in headers
    assert not any(name.lower().startswith("access-control") for name in headers)
