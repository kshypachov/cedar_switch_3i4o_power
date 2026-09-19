# SPDX-License-Identifier: Apache-2.0
"""Level one: every declared operation answers, and its answer fits the schema.

This is the contract test section 12 asks for — "one set of examples is run
against the mock server of stage P1 and against the firmware at the hardware
tier; a response that diverges from the schema is a test failure, not a remark".
The walkthrough below reaches all forty operations in dependency order,
because half of them need something to exist first: a job to poll, a transaction
to apply, an upload to verify.

The schema check itself is not here. It is in `Client._check`, which runs on
every call in every test file, so this module only has to prove that all
forty were reached.
"""

from __future__ import annotations

import hashlib
from typing import Any

from cedar_contract.mock.constants import (
    FIRMWARE_VERIFY_MS,
    QUEUE_MS,
    UPLOAD_CHUNK_MS,
    WIFI_SCAN_MS,
)
from cedar_contract.mock.mcuboot import build_image
from cedar_contract.mock.wire import Request
from cedar_contract.openapi import Document

from .conftest import Harness, build


def _walk(harness: Harness) -> set[str]:
    """Call all forty operations, in an order that makes each one legal."""
    client = harness.client
    seconds = 1 / 1000

    # Session. The device starts unconfigured, so setup comes before login.
    client.get("/auth/state", authenticate=False)
    client.setup()
    client.get("/auth/session")

    # Device and capabilities.
    client.get("/system/status")
    client.get("/capabilities")
    client.get("/system/coredump")
    harness.app.handle(Request("POST", "/__mock/crash", {}, b""))
    client.login()
    client.get("/system/coredump/data")
    client.delete("/system/coredump")
    client.get("/coprocessor/status")

    # Matter: open a window, poll the job, read the codes and the fabrics.
    client.get("/matter/status")
    client.get("/matter/commissioning")
    opened = client.post("/matter/commissioning", {"mode": "basic", "timeout_seconds": 300})
    client.get(f"/jobs/{opened.json['job_id']}")
    client.get("/matter/onboarding-codes")
    client.get("/matter/fabrics")
    client.delete("/matter/commissioning")

    # Wi-Fi scan, and a job that can be cancelled.
    scan = client.post("/network/wifi/scans", {})
    client.get(f"/network/wifi/scans/{scan.json['job_id']}")
    client.post(f"/jobs/{scan.json['job_id']}/cancel", {})

    # Network: stage, apply, confirm; then a second candidate to discard.
    from .conftest import VALID_CANDIDATE

    revision = client.get("/network/config").json["revision"]
    client.get("/network/status")
    staged = client.post(
        "/network/transactions", {"base_revision": revision, "config": VALID_CANDIDATE}
    )
    transaction = staged.json["id"]
    client.get(f"/network/transactions/{transaction}")
    client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    harness.advance(5)
    client.post(f"/network/transactions/{transaction}/confirm", {})
    harness.advance(5)
    revision = client.get("/network/config").json["revision"]
    staged = client.post(
        "/network/transactions", {"base_revision": revision, "config": VALID_CANDIDATE}
    )
    client.delete(f"/network/transactions/{staged.json['id']}")
    harness.advance(1)

    # Logs.
    client.get("/logs/sources")
    client.get("/logs/records?limit=10")
    client.get("/logs/export?format=ndjson&max_records=20")

    # Firmware: create, one chunk, verify, install, then delete the file.
    size = 512
    upload = client.post(
        "/firmware/uploads",
        {"filename": "cedar-c6-1.4.2.bin", "size_bytes": size, "sha256": "0" * 64},
    )
    upload_id = upload.json["id"]
    client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"\x00" * size)
    harness.advance(UPLOAD_CHUNK_MS * seconds + 0.1)
    client.get(f"/firmware/uploads/{upload_id}")
    client.get("/firmware/uploads")
    client.post(f"/firmware/uploads/{upload_id}/verify", {})
    harness.advance((QUEUE_MS + FIRMWARE_VERIFY_MS) * seconds + 0.1)
    client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": "uart", "acknowledge_recovery": True},
    )
    harness.advance(30)
    client.delete(f"/firmware/uploads/{upload_id}")
    harness.advance(1)

    # The STM32 image: upload, verify, install. The install restarts the device
    # once its phases are over (a little over five seconds), so nothing below
    # waits that long.
    image = build_image(b"\x00" * 64)
    upload = client.post(
        "/firmware/uploads",
        {
            "filename": "zephyr.signed.bin",
            "size_bytes": len(image),
            "sha256": hashlib.sha256(image).hexdigest(),
            "target": "stm32u585",
        },
    )
    upload_id = upload.json["id"]
    client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=image)
    harness.advance(UPLOAD_CHUNK_MS * seconds + 0.1)
    client.post(f"/firmware/uploads/{upload_id}/verify", {})
    harness.advance((QUEUE_MS + FIRMWARE_VERIFY_MS) * seconds + 0.1)
    client.get("/system/firmware")
    client.post("/system/updates", {"upload_id": upload_id, "acknowledge_downgrade": False})

    # Password change, then the session operations it invalidates: the change
    # revokes every session, so login has to come after it and logout after that.
    client.put(
        "/auth/password",
        {"current_password": "cedar-mock-admin", "new_password": "a-longer-password"},
    )
    harness.advance(2)
    assert client.get("/system/status").status == 401, "the change revoked the session"
    assert client.login("a-longer-password").status == 200
    client.delete("/auth/session")

    _ = WIFI_SCAN_MS
    return {op for op, _status in client.seen}


def test_every_declared_operation_is_answered(document: Document) -> None:
    harness = build(document)
    reached = _walk(harness)
    declared = set(document.operation_ids())
    assert declared - reached == set(), "operations the walkthrough never called"
    assert reached == declared


def test_the_mock_has_a_handler_for_every_declared_operation(document: Document) -> None:
    """Reported as a set rather than found one 404 at a time, and the property is
    what P2 will diff the device's route table against."""
    assert build(document).app.uncovered == ()


def test_every_answer_used_the_status_the_document_declares(document: Document) -> None:
    """Each operation declares exactly one success status; the walkthrough must
    have produced that one, not merely something in the 2xx range."""
    harness = build(document)
    _walk(harness)
    statuses: dict[str, set[int]] = {}
    for operation_id, status in harness.client.seen:
        statuses.setdefault(operation_id, set()).add(status)
    for operation_id, seen in statuses.items():
        declared = int(document.operation(operation_id).success_status)
        assert declared in seen, f"{operation_id} was never answered with {declared}: {seen}"


def test_a_handler_for_an_undeclared_operation_cannot_be_registered(document: Document) -> None:
    """The router is built from the document, so the failure is at construction
    rather than at the first request."""
    from cedar_contract.mock import wire
    from cedar_contract.mock.app import MockApp

    wire.HANDLERS["notAnOperation"] = lambda app, ctx: None  # type: ignore[assignment,return-value]
    try:
        try:
            MockApp(document=document)
        except RuntimeError as exc:
            assert "notAnOperation" in str(exc)
        else:
            raise AssertionError("an undeclared handler was accepted")
    finally:
        del wire.HANDLERS["notAnOperation"]


def test_base64_content_encoding_is_checked_by_the_service(document: Document) -> None:
    """`contentEncoding` is annotation-only in 2020-12, so `jsonschema` ignores it.

    The check therefore has to live somewhere, and it lives in the service rather
    than in the request middleware: the only base64 field in any request is the
    SSID, and the rule about it is a business rule anyway — 0-32 decoded bytes,
    non-empty when Wi-Fi is enabled. Putting it there also gets the field path
    right, which a generic walk over the schema would not.
    """
    from .conftest import candidate

    harness = build(document, setup_required=False)
    harness.client.login()
    revision = harness.client.get("/network/config").json["revision"]
    broken: dict[str, Any] = candidate(
        **{
            "interfaces/wifi/enabled": True,
            "interfaces/wifi/ssid_base64": "not base64!!",
            "interfaces/wifi/security": "wpa2_psk",
            "interfaces/wifi/credential": {"action": "replace", "value": "hunter22hunter"},
        }
    )
    response = harness.client.post(
        "/network/transactions", {"base_revision": revision, "config": broken}
    )
    assert response.status == 422
    detail = response.json["error"]
    assert detail["code"] == "validation_failed"
    assert detail["fields"] == [
        {"path": "/config/interfaces/wifi/ssid_base64", "code": "invalid_format"}
    ]
