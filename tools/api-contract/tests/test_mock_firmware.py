# SPDX-License-Identifier: Apache-2.0
"""Upload, verification, and the UART install — with the offset rule intact.

`received_bytes` is the contract's next acceptable offset *after a crash*, so it
may only move once the chunk is on storage. That is the one thing here a static
mock would get wrong in a way the frontend would inherit: a resume path built
against a counter that advanced on arrival skips a chunk the device never wrote.

The other is the OTA refusal. It is the owner's decision that the first version
has no OTA, and the contract says the UI must show the server's reason rather
than a disabled placeholder switch — which only works if the reason is actually
sent.
"""

from __future__ import annotations

from cedar_contract.mock.constants import (
    FIRMWARE_DELETE_MS,
    FIRMWARE_VERIFY_MS,
    QUEUE_MS,
    UPLOAD_CHUNK_BYTES,
    UPLOAD_CHUNK_MS,
    UPLOAD_MAX_BYTES,
)
from cedar_contract.mock.firmware import Coprocessor
from cedar_contract.openapi import Document

from .conftest import Harness, build

SECOND = 1 / 1000
FLUSHED = UPLOAD_CHUNK_MS * SECOND + 0.05
VERIFIED = (QUEUE_MS + FIRMWARE_VERIFY_MS) * SECOND + 0.1
SHA = "0" * 64


def create(harness: Harness, size: int = 600, **changes: object) -> dict:
    body = {"filename": "cedar-c6-1.4.2.bin", "size_bytes": size, "sha256": SHA}
    body.update(changes)
    return harness.client.post("/firmware/uploads", body)


def upload_all(harness: Harness, size: int = 600) -> str:
    upload_id = create(harness, size).json["id"]
    offset = 0
    while offset < size:
        chunk = min(256, size - offset)
        harness.client.put(
            f"/firmware/uploads/{upload_id}/data?offset={offset}", raw=b"\x00" * chunk
        )
        harness.advance(FLUSHED)
        offset += chunk
    return upload_id


def ready(harness: Harness, size: int = 600) -> str:
    upload_id = upload_all(harness, size)
    harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
    harness.advance(VERIFIED)
    return upload_id


# -- upload ----------------------------------------------------------------


def test_an_upload_starts_empty_with_a_location(harness: Harness) -> None:
    response = create(harness)
    assert response.status == 201
    body = response.json
    assert response.headers["Location"] == f"/api/v1/firmware/uploads/{body['id']}"
    assert body["received_bytes"] == 0
    assert body["state"] == "receiving"
    assert body["image"] is None, "nothing is known about the file before verification"


def test_the_client_filename_is_a_display_value(harness: Harness) -> None:
    """The contract forbids using it as a path. A frontend can therefore send an
    awkward one, and the server must simply carry it back."""
    response = create(harness, filename="../../etc/passwd")
    assert response.status == 201
    assert response.json["filename"] == "../../etc/passwd"
    assert response.json["id"].startswith("upload_")


def test_received_bytes_advances_only_when_the_chunk_is_flushed(harness: Harness) -> None:
    upload_id = create(harness).json["id"]
    accepted = harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * 256)
    assert accepted.status == 202
    during = harness.client.get(f"/firmware/uploads/{upload_id}").json
    assert during["received_bytes"] == 0, "the bytes have arrived but are not on storage"
    assert during["active_job_id"] == accepted.json["job_id"]
    harness.advance(FLUSHED)
    after = harness.client.get(f"/firmware/uploads/{upload_id}").json
    assert after["received_bytes"] == 256
    assert after["active_job_id"] is None


def test_a_second_chunk_before_the_first_is_written_is_busy(harness: Harness) -> None:
    """The contract says the next chunk is not accepted until the job finishes."""
    upload_id = create(harness).json["id"]
    harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * 256)
    second = harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=256", raw=b"y" * 256)
    assert second.status == 409
    assert second.json["error"]["code"] == "busy"


def test_a_wrong_offset_is_offset_mismatch_and_says_the_right_one(harness: Harness) -> None:
    upload_id = create(harness).json["id"]
    harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * 256)
    harness.advance(FLUSHED)
    for offset in (0, 128, 300):
        response = harness.client.put(
            f"/firmware/uploads/{upload_id}/data?offset={offset}", raw=b"y" * 64
        )
        assert response.status == 409, offset
        assert response.json["error"]["code"] == "offset_mismatch"
        assert "256" in response.json["error"]["message"]
    assert harness.client.get(f"/firmware/uploads/{upload_id}").json["received_bytes"] == 256, (
        "nothing was overwritten silently"
    )


def test_a_resume_after_a_lost_answer_uses_the_authoritative_offset(harness: Harness) -> None:
    """The contract's recovery: poll first, then send only the missing chunk."""
    upload_id = create(harness, 512).json["id"]
    harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * 256)
    harness.advance(FLUSHED)
    offset = harness.client.get(f"/firmware/uploads/{upload_id}").json["received_bytes"]
    assert (
        harness.client.put(
            f"/firmware/uploads/{upload_id}/data?offset={offset}", raw=b"y" * 256
        ).status
        == 202
    )
    harness.advance(FLUSHED)
    assert harness.client.get(f"/firmware/uploads/{upload_id}").json["received_bytes"] == 512


def test_a_chunk_past_the_declared_size_is_refused(harness: Harness) -> None:
    upload_id = create(harness, 100).json["id"]
    response = harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * 200)
    assert response.status == 422
    assert "past the declared size" in response.json["error"]["message"]


def test_a_chunk_over_the_published_limit_is_too_large(harness: Harness) -> None:
    """The limit comes from `capabilities`, and the mock enforces the number it
    publishes rather than a different one."""
    limit = harness.client.get("/capabilities").json["limits"]["upload_chunk_bytes"]
    assert limit == UPLOAD_CHUNK_BYTES
    upload_id = create(harness, limit * 2).json["id"]
    response = harness.client.put(
        f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * (limit + 1)
    )
    assert response.status == 413
    assert response.json["error"]["code"] == "payload_too_large"


def test_an_image_larger_than_the_limit_is_refused_before_any_bytes(harness: Harness) -> None:
    response = create(harness, UPLOAD_MAX_BYTES + 1)
    assert response.status == 413
    assert response.json["error"]["code"] == "payload_too_large"


def test_storage_full_is_reported_up_front(document: Document) -> None:
    """The contract requires the space check before the first byte, with the quota
    held for the whole upload — not a failed write halfway through."""
    harness = build(document, setup_required=False, storage_free_bytes=1024)
    harness.client.login()
    response = create(harness, 4096)
    assert response.status == 507
    assert response.json["error"]["code"] == "storage_full"


def test_only_one_upload_exists_at_a_time(harness: Harness) -> None:
    """And the contract forbids the server deleting a ready package on its own
    initiative, so a new file needs the old one deleted first."""
    first = create(harness).json["id"]
    second = create(harness)
    assert second.status == 409
    assert second.json["error"]["code"] == "busy"
    assert first in second.json["error"]["message"]


def test_an_unknown_upload_is_not_found(harness: Harness) -> None:
    assert harness.client.get("/firmware/uploads/upload_ffff").status == 404


# -- verification ----------------------------------------------------------


def test_verifying_an_incomplete_upload_is_invalid_state(harness: Harness) -> None:
    upload_id = create(harness, 600).json["id"]
    harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * 256)
    harness.advance(FLUSHED)
    response = harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
    assert response.status == 409
    assert "256 of 600" in response.json["error"]["message"]


def test_verification_reaches_ready_and_describes_the_image(harness: Harness) -> None:
    upload_id = upload_all(harness)
    accepted = harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
    assert accepted.status == 202
    assert harness.client.get(f"/firmware/uploads/{upload_id}").json["state"] == "verifying"
    harness.advance(VERIFIED)
    body = harness.client.get(f"/firmware/uploads/{upload_id}").json
    assert body["state"] == "ready"
    image = body["image"]
    assert image["format"] == "raw_app"
    assert image["target"] == "esp32c6"
    assert image["allowed_methods"] == ["uart"], "OTA is not offered for any image"
    assert image["signature_verified"] is None, (
        "a raw application image carries no signature, and false would claim a "
        "check was made and failed"
    )


def test_a_failed_verification_leaves_the_reason_on_the_upload(document: Document) -> None:
    for outcome in ("invalid_image", "unsupported_target", "incompatible_firmware"):
        harness = build(document, setup_required=False, verify_result=outcome)
        harness.client.login()
        upload_id = upload_all(harness)
        accepted = harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
        harness.advance(VERIFIED)
        body = harness.client.get(f"/firmware/uploads/{upload_id}").json
        assert body["state"] == "failed", outcome
        assert body["image"] is None
        assert body["error"]["code"] == outcome
        assert harness.client.get(f"/jobs/{accepted.json['job_id']}").json["state"] == "failed"


def test_a_failed_upload_may_be_replaced(document: Document) -> None:
    """The one case where a second upload is allowed without a delete: there is
    nothing worth keeping."""
    harness = build(document, setup_required=False, verify_result="invalid_image")
    harness.client.login()
    upload_all(harness)
    harness.client.post(f"/firmware/uploads/{harness.state.firmware.upload.id}/verify", {})
    harness.advance(VERIFIED)
    assert create(harness).status == 201


# -- delete ----------------------------------------------------------------


def test_deleting_an_upload_is_a_cleanup_job(harness: Harness) -> None:
    upload_id = ready(harness)
    accepted = harness.client.delete(f"/firmware/uploads/{upload_id}")
    assert accepted.status == 202
    harness.advance(FIRMWARE_DELETE_MS * SECOND + 0.1)
    assert harness.client.get(f"/firmware/uploads/{upload_id}").status == 404
    assert create(harness).status == 201


def test_deleting_a_file_an_install_is_using_is_busy(harness: Harness) -> None:
    upload_id = ready(harness)
    harness.client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": "uart", "acknowledge_recovery": False},
    )
    response = harness.client.delete(f"/firmware/uploads/{upload_id}")
    assert response.status == 409
    assert response.json["error"]["code"] == "busy"


# -- the install -----------------------------------------------------------


def test_ota_is_refused_with_the_reason_the_ui_must_show(harness: Harness) -> None:
    upload_id = ready(harness)
    response = harness.client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": "ota", "acknowledge_recovery": False},
    )
    assert response.status == 503
    assert response.json["error"]["code"] == "capability_unavailable"
    assert response.json["error"]["retryable"] is False, (
        "waiting does not add a capability the build does not have"
    )


def test_capabilities_and_the_coprocessor_agree_that_ota_is_not_implemented(
    harness: Harness,
) -> None:
    """The owner's decision, in the two places the UI reads it. `reason` is a
    string from the server precisely so the screen shows a reason instead of a
    greyed-out control."""
    features = harness.client.get("/capabilities").json["features"]
    assert features["esp32_ota"] == {"available": False, "reason": "not_implemented"}
    assert features["esp32_uart"]["available"] is True
    status = harness.client.get("/coprocessor/status").json
    assert status["ota"] == {"available": False, "reason": Coprocessor.OTA_REASON}
    assert status["uart_update"]["available"] is True
    assert harness.client.get("/capabilities").json["update_requires_ethernet"] is True


def test_installing_an_unverified_image_is_invalid_state(harness: Harness) -> None:
    upload_id = upload_all(harness)
    response = harness.client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": "uart", "acknowledge_recovery": False},
    )
    assert response.status == 409
    assert "'receiving'" in response.json["error"]["message"]


def test_a_request_that_did_not_arrive_over_ethernet_is_refused(document: Document) -> None:
    """`ethernet_required`, before any reset or erase, as the contract insists.
    The mock cannot observe the transport, so the scenario stands in for it —
    which is the only way the frontend gets to build this path at all."""
    harness = build(document, setup_required=False, install_transport="wifi")
    harness.client.login()
    upload_id = ready(harness)
    response = harness.client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": "uart", "acknowledge_recovery": False},
    )
    assert response.status == 409
    assert response.json["error"]["code"] == "ethernet_required"


def test_a_successful_install_updates_the_version_and_the_summary(harness: Harness) -> None:
    upload_id = ready(harness)
    before = harness.client.get("/coprocessor/status").json
    accepted = harness.client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": "uart", "acknowledge_recovery": False},
    )
    during = harness.client.get("/coprocessor/status").json
    assert during["state"] == "updating"
    assert during["uart_mode"] == "flashing"
    assert during["transport_ready"] is False
    harness.advance(30)
    after = harness.client.get("/coprocessor/status").json
    assert after["state"] == "ready"
    assert after["uart_mode"] == "console"
    assert after["firmware_version"] == "1.4.2" != before["firmware_version"]
    assert after["generation"] == before["generation"] + 1, "a reset means a new generation"
    assert after["last_update"] == {
        "job_id": accepted.json["job_id"],
        "state": "succeeded",
        "method": "uart",
        "version": "1.4.2",
        "recovery_required": False,
        "error": None,
    }


def test_a_coprocessor_that_does_not_come_back_reports_recovery_required(
    document: Document,
) -> None:
    """The contract is explicit: if the C6 did not revive, that is an error with
    `recovery_required`, not a hundred-percent success."""
    harness = build(document, setup_required=False, install_result="recovery_required")
    harness.client.login()
    upload_id = ready(harness)
    accepted = harness.client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": "uart", "acknowledge_recovery": False},
    )
    harness.advance(30)
    job = harness.client.get(f"/jobs/{accepted.json['job_id']}").json
    assert job["state"] == "failed"
    assert job["error"] is not None
    summary = harness.client.get("/coprocessor/status").json["last_update"]
    assert summary["state"] == "failed"
    assert summary["recovery_required"] is True
    assert summary["version"] is None


def test_a_second_install_while_one_runs_is_busy(harness: Harness) -> None:
    upload_id = ready(harness)
    body = {"upload_id": upload_id, "method": "uart", "acknowledge_recovery": False}
    harness.client.post("/coprocessor/updates", body)
    second = harness.client.post("/coprocessor/updates", body)
    assert second.status == 409
    assert second.json["error"]["code"] == "busy"


def test_an_offline_coprocessor_refuses_an_install_and_says_why(document: Document) -> None:
    harness = build(document, setup_required=False, coprocessor_state="offline")
    harness.client.login()
    upload_id = ready(harness)
    status = harness.client.get("/coprocessor/status").json
    assert status["state"] == "offline"
    assert status["firmware_version"] is None
    # The UART's owner does not follow the C6's answer over ESP-Hosted (P5): the
    # console keeps it, as on board B whose C6 has no firmware.
    assert status["uart_mode"] == "console"
    assert status["uart_update"]["available"] is False
    assert status["uart_update"]["reason"], "unavailable always comes with a reason"
    response = harness.client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": "uart", "acknowledge_recovery": False},
    )
    assert response.status == 503
    assert response.json["error"]["code"] == "service_not_ready"
    assert response.json["error"]["retryable"] is True
