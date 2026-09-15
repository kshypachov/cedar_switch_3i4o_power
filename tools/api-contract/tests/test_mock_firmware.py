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

P6 made the file a whole flash image and the install the path for an empty
coprocessor; the tests from "the install" on follow the device's rules for that.
"""

from __future__ import annotations

from cedar_contract.mock.constants import (
    FIRMWARE_DELETE_MS,
    FIRMWARE_VERIFY_MS,
    INSTALL_PHASE_MS,
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
#: Into `preflight`, safely before `begin`.
BEFORE_BEGIN = (QUEUE_MS + INSTALL_PHASE_MS // 2) * SECOND
#: Into `writing`: queued, preflight, entering_bootloader, begin, then half a phase.
WRITING = (QUEUE_MS + INSTALL_PHASE_MS * 3 + INSTALL_PHASE_MS // 2) * SECOND
SHA = "0" * 64


def create(harness: Harness, size: int = 600, **changes: object) -> dict:
    body = {"filename": "cedar-c6-merged.bin", "size_bytes": size, "sha256": SHA}
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


def install(harness: Harness, upload_id: str, acknowledge: bool = True, method: str = "uart"):
    return harness.client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": method, "acknowledge_recovery": acknowledge},
    )


def signed_in(document: Document, **scenario: object) -> Harness:
    harness = build(document, setup_required=False, **scenario)
    harness.client.login()
    return harness


def reboot(harness: Harness) -> None:
    harness.app.reboot()
    assert harness.client.login().status == 200


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


def test_an_empty_chunk_writes_nothing(harness: Harness) -> None:
    upload_id = create(harness).json["id"]
    response = harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"")
    assert response.status == 422
    assert response.json["error"]["code"] == "validation_failed"
    assert harness.client.get(f"/firmware/uploads/{upload_id}").json["active_job_id"] is None


def test_a_chunk_that_is_not_binary_is_unsupported_media_type(harness: Harness) -> None:
    upload_id = create(harness).json["id"]
    response = harness.client.request(
        "PUT",
        f"/firmware/uploads/{upload_id}/data?offset=0",
        raw=b"x" * 64,
        headers={"content-type": "text/plain"},
    )
    assert response.status == 415
    assert response.json["error"]["code"] == "unsupported_media_type"


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
    assert limit == UPLOAD_CHUNK_BYTES == 16_384
    upload_id = create(harness, limit * 2).json["id"]
    response = harness.client.put(
        f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * (limit + 1)
    )
    assert response.status == 413
    assert response.json["error"]["code"] == "payload_too_large"


def test_the_largest_image_ends_where_the_second_slot_begins(harness: Harness) -> None:
    """A whole flash image written from 0x0 must stop before `ota_1` at 0x1d0000;
    the device publishes that as the limit and refuses a larger file before any
    bytes, and a file of exactly that size is still accepted."""
    limits = harness.client.get("/capabilities").json["limits"]
    assert limits["upload_max_bytes"] == UPLOAD_MAX_BYTES == 1_900_544
    response = create(harness, UPLOAD_MAX_BYTES + 1)
    assert response.status == 413
    assert response.json["error"]["code"] == "payload_too_large"
    assert create(harness, UPLOAD_MAX_BYTES).status == 201


def test_storage_full_is_reported_up_front(document: Document) -> None:
    """The contract requires the space check before the first byte, with the quota
    held for the whole upload — not a failed write halfway through."""
    harness = signed_in(document, storage_free_bytes=1024)
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


def test_a_reboot_keeps_the_file_up_to_the_last_flushed_chunk(harness: Harness) -> None:
    """The file lives on LittleFS. A chunk whose job had not finished is not
    counted, because `received_bytes` is only ever what reached storage."""
    upload_id = create(harness, 600).json["id"]
    harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * 256)
    harness.advance(FLUSHED)
    harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=256", raw=b"y" * 256)
    reboot(harness)
    body = harness.client.get(f"/firmware/uploads/{upload_id}").json
    assert body["received_bytes"] == 256
    assert body["active_job_id"] is None
    assert body["state"] == "receiving"
    assert (
        harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=256", raw=b"y" * 256).status
        == 202
    )


def test_a_reboot_during_verification_leaves_the_file_to_verify_again(harness: Harness) -> None:
    upload_id = upload_all(harness)
    harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
    reboot(harness)
    body = harness.client.get(f"/firmware/uploads/{upload_id}").json
    assert body["state"] == "receiving"
    assert body["active_job_id"] is None
    assert harness.client.post(f"/firmware/uploads/{upload_id}/verify", {}).status == 202


# -- verification ----------------------------------------------------------


def test_verifying_an_incomplete_upload_is_invalid_state(harness: Harness) -> None:
    upload_id = create(harness, 600).json["id"]
    harness.client.put(f"/firmware/uploads/{upload_id}/data?offset=0", raw=b"x" * 256)
    harness.advance(FLUSHED)
    response = harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
    assert response.status == 409
    assert "256 of 600" in response.json["error"]["message"]


def test_verification_describes_a_whole_flash_image(harness: Harness) -> None:
    upload_id = upload_all(harness)
    accepted = harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
    assert accepted.status == 202
    assert harness.client.get(f"/firmware/uploads/{upload_id}").json["state"] == "verifying"
    harness.advance(VERIFIED)
    body = harness.client.get(f"/firmware/uploads/{upload_id}").json
    assert body["state"] == "ready"
    assert body["image"] == {
        "format": "raw_full_flash",
        "format_version": None,
        "target": "esp32c6",
        "version": "1",
        "kind": "recovery_bundle",
        "partition_layout_id": "cedar-c6-ota-4m-2x1792k",
        "host_protocol": "esp-hosted-mcu-3",
        # A raw file carries no signature, and false would claim a check was
        # made and failed.
        "signature_verified": None,
        "allowed_methods": ["uart"],
    }


def test_capabilities_name_the_accepted_formats(harness: Harness) -> None:
    """The coprocessor's whole flash image, and since the STM32 update stage the
    MCUboot image of the STM32 itself."""
    assert harness.client.get("/capabilities").json["firmware_formats"] == [
        "raw_full_flash",
        "mcuboot_image",
    ]


def test_a_failed_verification_leaves_the_reason_on_the_upload(document: Document) -> None:
    expected = {
        "invalid_image": ("invalid_image", "not a whole"),
        "bare_app": ("invalid_image", "merged file (idf.py merge-bin)"),
        "unsupported_target": ("unsupported_target", "different chip"),
        "incompatible_firmware": ("incompatible_firmware", "partition table"),
    }
    for outcome, (code, words) in expected.items():
        harness = signed_in(document, verify_result=outcome)
        upload_id = upload_all(harness)
        accepted = harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
        harness.advance(VERIFIED)
        body = harness.client.get(f"/firmware/uploads/{upload_id}").json
        assert body["state"] == "failed", outcome
        assert body["image"] is None
        assert body["error"]["code"] == code, outcome
        assert words in body["error"]["message"], outcome
        job = harness.client.get(f"/jobs/{accepted.json['job_id']}").json
        assert job["state"] == "failed"
        assert job["error"]["code"] == code


def test_a_failed_upload_may_be_replaced(document: Document) -> None:
    """The one case where a second upload is allowed without a delete: there is
    nothing worth keeping."""
    harness = signed_in(document, verify_result="invalid_image")
    upload_all(harness)
    harness.client.post(f"/firmware/uploads/{harness.state.firmware.upload.id}/verify", {})
    harness.advance(VERIFIED)
    assert create(harness).status == 201


def test_a_cancelled_verification_leaves_the_file_to_verify_again(harness: Harness) -> None:
    upload_id = upload_all(harness)
    accepted = harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
    job_id = accepted.json["job_id"]
    assert harness.client.get(f"/jobs/{job_id}").json["cancellable"] is True
    assert harness.client.post(f"/jobs/{job_id}/cancel", {}).status == 202
    assert harness.client.get(f"/jobs/{job_id}").json["state"] == "cancelled"
    body = harness.client.get(f"/firmware/uploads/{upload_id}").json
    assert body["state"] == "receiving"
    assert body["active_job_id"] is None
    assert harness.client.post(f"/firmware/uploads/{upload_id}/verify", {}).status == 202


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
    assert install(harness, upload_id).status == 202
    response = harness.client.delete(f"/firmware/uploads/{upload_id}")
    assert response.status == 409
    assert response.json["error"]["code"] == "busy"


# -- the install -----------------------------------------------------------


def test_ota_is_refused_with_the_reason_the_ui_must_show(harness: Harness) -> None:
    upload_id = ready(harness)
    response = install(harness, upload_id, method="ota")
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
    assert features["esp32_uart"] == {"available": True, "reason": None}
    status = harness.client.get("/coprocessor/status").json
    assert status["ota"] == {"available": False, "reason": Coprocessor.OTA_REASON}
    assert status["uart_update"] == {"available": True, "reason": None}
    assert harness.client.get("/capabilities").json["update_requires_ethernet"] is True


def test_an_image_that_replaces_the_bootloader_needs_acknowledge_recovery(
    harness: Harness,
) -> None:
    """Every accepted file is a recovery_bundle: it rewrites the bootloader and
    the partition table and erases the coprocessor's NVS. Without the flag
    nothing starts."""
    upload_id = ready(harness)
    response = install(harness, upload_id, acknowledge=False)
    assert response.status == 422
    assert response.json["error"]["code"] == "validation_failed"
    assert "acknowledge_recovery" in response.json["error"]["message"]
    status = harness.client.get("/coprocessor/status").json
    assert status["uart_mode"] == "console", "no job started"
    assert status["last_update"] is None
    assert harness.client.get("/system/status").json["active_job_ids"] == []


def test_installing_an_unverified_image_is_invalid_state(harness: Harness) -> None:
    upload_id = upload_all(harness)
    response = install(harness, upload_id)
    assert response.status == 409
    assert response.json["error"]["code"] == "invalid_state"
    assert "'receiving'" in response.json["error"]["message"]


def test_a_request_that_did_not_arrive_over_ethernet_is_refused(document: Document) -> None:
    """`ethernet_required`, before any reset or erase, as the contract insists.
    The mock cannot observe the transport, so the scenario stands in for it —
    which is the only way the frontend gets to build this path at all."""
    harness = signed_in(document, install_transport="wifi")
    upload_id = ready(harness)
    response = install(harness, upload_id)
    assert response.status == 409
    assert response.json["error"]["code"] == "ethernet_required"


def test_a_successful_install_updates_the_version_and_the_summary(harness: Harness) -> None:
    upload_id = ready(harness)
    before = harness.client.get("/coprocessor/status").json
    accepted = install(harness, upload_id)
    during = harness.client.get("/coprocessor/status").json
    assert during["state"] == "updating"
    assert during["uart_mode"] == "flashing"
    assert during["transport_ready"] is False
    assert during["uart_update"] == {"available": False, "reason": "uart_flashing"}
    assert harness.client.get("/capabilities").json["features"]["esp32_uart"] == {
        "available": False,
        "reason": "uart_flashing",
    }
    harness.advance(30)
    after = harness.client.get("/coprocessor/status").json
    assert after["state"] == "ready"
    assert after["uart_mode"] == "console"
    assert after["transport_ready"] is True
    assert after["firmware_version"] == before["firmware_version"] == "v3.0.6"
    assert after["host_protocol"] == "esp-hosted-mcu-3"
    assert after["partition_layout_id"] == "cedar-c6-ota-4m-2x1792k"
    assert after["generation"] == before["generation"] + 1, "a reset means a new generation"
    assert after["last_update"] == {
        "job_id": accepted.json["job_id"],
        "state": "succeeded",
        "method": "uart",
        "version": "1",
        "recovery_required": False,
        "error": None,
    }


def test_a_coprocessor_that_does_not_come_back_reports_recovery_required(
    document: Document,
) -> None:
    """The contract is explicit: if the C6 did not revive, that is an error with
    `recovery_required`, not a hundred-percent success."""
    harness = signed_in(document, install_result="recovery_required")
    upload_id = ready(harness)
    accepted = install(harness, upload_id)
    harness.advance(30)
    job = harness.client.get(f"/jobs/{accepted.json['job_id']}").json
    assert job["state"] == "failed"
    assert job["error"] is not None
    status = harness.client.get("/coprocessor/status").json
    assert status["state"] == "failed"
    summary = status["last_update"]
    assert summary["state"] == "failed"
    assert summary["recovery_required"] is True
    assert summary["version"] is None
    assert status["uart_update"]["available"] is True, "a retry is how recovery happens"


def test_a_second_install_while_one_runs_is_busy(harness: Harness) -> None:
    upload_id = ready(harness)
    install(harness, upload_id)
    second = install(harness, upload_id)
    assert second.status == 409
    assert second.json["error"]["code"] == "busy"


def test_an_offline_coprocessor_is_installed_all_the_same(document: Document) -> None:
    """The first flash of an empty chip: board B's C6 has no firmware at all, and
    writing a whole image is exactly how it gets some. Its state does not gate
    the install; the UART's owner does."""
    for state in ("offline", "failed"):
        harness = signed_in(document, coprocessor_state=state)
        upload_id = ready(harness)
        status = harness.client.get("/coprocessor/status").json
        assert status["state"] == state
        assert status["firmware_version"] is None
        assert status["uart_mode"] == "console"
        assert status["uart_update"] == {"available": True, "reason": None}, state
        assert install(harness, upload_id).status == 202, state
        harness.advance(30)
        after = harness.client.get("/coprocessor/status").json
        assert after["state"] == "ready", state
        assert after["firmware_version"] == "v3.0.6"
        assert after["transport_ready"] is True


def test_a_uart_bridged_to_usb_refuses_an_install_and_says_why(document: Document) -> None:
    harness = signed_in(document, uart_mode="usb_bridge")
    upload_id = ready(harness)
    unavailable = {"available": False, "reason": "uart_usb_bridge"}
    assert harness.client.get("/coprocessor/status").json["uart_update"] == unavailable
    assert harness.client.get("/capabilities").json["features"]["esp32_uart"] == unavailable
    response = install(harness, upload_id)
    assert response.status == 409
    assert response.json["error"]["code"] == "busy"


def test_a_uart_nobody_can_own_refuses_an_install(document: Document) -> None:
    harness = signed_in(document, uart_mode="unavailable")
    upload_id = ready(harness)
    assert harness.client.get("/coprocessor/status").json["uart_update"] == {
        "available": False,
        "reason": "uart_unavailable",
    }
    response = install(harness, upload_id)
    assert response.status == 503
    assert response.json["error"]["code"] == "capability_unavailable"


def test_an_install_can_be_cancelled_until_it_begins_erasing(harness: Harness) -> None:
    upload_id = ready(harness)
    job_id = install(harness, upload_id).json["job_id"]
    harness.advance(BEFORE_BEGIN)
    job = harness.client.get(f"/jobs/{job_id}").json
    assert job["phase"] == "preflight"
    assert job["cancellable"] is True
    assert harness.client.post(f"/jobs/{job_id}/cancel", {}).status == 202
    assert harness.client.get(f"/jobs/{job_id}").json["state"] == "cancelled"
    status = harness.client.get("/coprocessor/status").json
    assert status["uart_mode"] == "console"
    assert status["last_update"] is None, "nothing reached the chip"
    assert harness.client.delete(f"/firmware/uploads/{upload_id}").status == 202, (
        "the file is free again"
    )


def test_an_install_that_is_writing_cannot_be_cancelled(harness: Harness) -> None:
    upload_id = ready(harness)
    job_id = install(harness, upload_id).json["job_id"]
    harness.advance(WRITING)
    job = harness.client.get(f"/jobs/{job_id}").json
    assert job["phase"] == "writing"
    assert job["cancellable"] is False
    response = harness.client.post(f"/jobs/{job_id}/cancel", {})
    assert response.status == 409
    assert response.json["error"]["code"] == "invalid_state"
    assert harness.client.get(f"/jobs/{job_id}").json["state"] == "running"


def test_a_reboot_during_the_write_is_interrupted_and_needs_recovery(harness: Harness) -> None:
    """After a restart the device reconciles instead of continuing a destructive
    install: the journal turns into an `interrupted` summary, no job runs, and
    the file stays ready for an explicit retry."""
    upload_id = ready(harness)
    job_id = install(harness, upload_id).json["job_id"]
    harness.advance(WRITING)
    reboot(harness)
    status = harness.client.get("/coprocessor/status").json
    assert status["state"] == "failed"
    assert status["uart_mode"] == "console"
    summary = status["last_update"]
    assert summary["job_id"] == job_id
    assert summary["state"] == "interrupted"
    assert summary["recovery_required"] is True
    assert summary["version"] is None
    assert summary["error"]["code"] == "boot_changed"
    assert harness.client.get("/system/status").json["active_job_ids"] == []
    assert harness.client.get(f"/firmware/uploads/{upload_id}").json["state"] == "ready"
    assert install(harness, upload_id).status == 202, "the retry is a new request"


def test_a_reboot_before_the_write_began_needs_no_recovery(harness: Harness) -> None:
    upload_id = ready(harness)
    install(harness, upload_id)
    harness.advance(BEFORE_BEGIN)
    reboot(harness)
    status = harness.client.get("/coprocessor/status").json
    assert status["state"] == "ready", "the chip was not touched"
    assert status["last_update"]["state"] == "interrupted"
    assert status["last_update"]["recovery_required"] is False
    assert harness.client.delete(f"/firmware/uploads/{upload_id}").status == 202, (
        "no install uses the file after the reboot"
    )


def test_a_reboot_in_the_begin_phase_already_needs_recovery(harness: Harness) -> None:
    """`begin` is where the flasher erases: the first destructive phase counts."""
    upload_id = ready(harness)
    job_id = install(harness, upload_id).json["job_id"]
    harness.advance((QUEUE_MS + INSTALL_PHASE_MS * 2 + INSTALL_PHASE_MS // 2) * SECOND)
    assert harness.client.get(f"/jobs/{job_id}").json["phase"] == "begin"
    reboot(harness)
    assert harness.client.get("/coprocessor/status").json["last_update"]["recovery_required"] is True


def test_a_reboot_after_the_install_keeps_its_outcome(harness: Harness) -> None:
    upload_id = ready(harness)
    install(harness, upload_id)
    harness.advance(30)
    reboot(harness)
    status = harness.client.get("/coprocessor/status").json
    assert status["last_update"]["state"] == "succeeded"
    assert status["last_update"]["version"] == "1"
    assert status["firmware_version"] == "v3.0.6"


def test_the_status_version_is_what_the_c6_reports_not_the_image_version(
    document: Document,
) -> None:
    """Two numbers (api-contract.md, P6 decisions): the summary carries the image's
    `app_desc.version`, the status carries what the C6 reports over ESP-Hosted,
    and only while the transport is up; `host_protocol` follows that version's
    major. An empty C6 reports nothing."""
    harness = signed_in(document, coprocessor_state="offline", hosted_version="v4.1.0")
    upload_id = ready(harness)
    empty = harness.client.get("/coprocessor/status").json
    assert empty["firmware_version"] is None
    assert empty["host_protocol"] is None
    install(harness, upload_id)
    during = harness.client.get("/coprocessor/status").json
    assert during["firmware_version"] is None, "no transport while the UART is flashing"
    assert during["host_protocol"] is None
    harness.advance(30)
    after = harness.client.get("/coprocessor/status").json
    assert after["last_update"]["version"] == "1"
    assert after["firmware_version"] == "v4.1.0" != after["last_update"]["version"]
    assert after["host_protocol"] == "esp-hosted-mcu-4"


def test_an_install_that_ended_unobserved_before_a_reboot_keeps_its_outcome(
    harness: Harness,
) -> None:
    """Nobody polled between the end of the install and the reboot: the outcome
    was reached all the same, and must not turn into `interrupted`."""
    upload_id = ready(harness)
    job_id = install(harness, upload_id).json["job_id"]
    harness.source.value += 30_000  # the clock moves; no request settles anything
    reboot(harness)
    summary = harness.client.get("/coprocessor/status").json["last_update"]
    assert summary["job_id"] == job_id
    assert summary["state"] == "succeeded"
