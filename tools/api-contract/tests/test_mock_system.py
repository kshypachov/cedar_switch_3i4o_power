# SPDX-License-Identifier: Apache-2.0
"""The STM32 update: an upload straight into slot 2, the MCUboot image check,
the install job, the swap across a restart, and the self-confirmation.

api-contract.md, "Обновление STM32". The parts a frontend would otherwise never
meet: the device vanishing for the swap, a session that does not survive it, a
job that is 404 afterwards, and an outcome that is only decided twenty minutes
later - or undone by a power cut before then.
"""

from __future__ import annotations

import hashlib
import struct
import threading
import urllib.error
import urllib.request
from http.server import HTTPServer
from typing import Any

import pytest
from cedar_contract.mock import mcuboot
from cedar_contract.mock.app import MockApp
from cedar_contract.mock.clock import Clock, FrozenSource
from cedar_contract.mock.constants import (
    FIRMWARE_VERIFY_MS,
    QUEUE_MS,
    SYSTEM_UPLOAD_MAX_BYTES,
    UPLOAD_CHUNK_BYTES,
    UPLOAD_CHUNK_MS,
)
from cedar_contract.mock.jobs import Step
from cedar_contract.mock.scenario import Scenario
from cedar_contract.mock.server import _Handler
from cedar_contract.mock.wire import Request
from cedar_contract.openapi import Document

from .conftest import VALID_CANDIDATE, Harness, build

SECOND = 1 / 1000
FLUSHED = UPLOAD_CHUNK_MS * SECOND + 0.05
VERIFIED = (QUEUE_MS + FIRMWARE_VERIFY_MS) * SECOND + 0.1
PHASE = 1.5
REBOOT_DELAY = 2.0
SWAP = 5.0
#: From the install request to the restart: queued, preparing, requesting, rebooting.
TO_RESTART = QUEUE_MS * SECOND + 2 * PHASE + REBOOT_DELAY
STM32 = "stm32u585"
ESP_SHA = "0" * 64


def signed_in(document: Document, **scenario: Any) -> Harness:
    harness = build(document, setup_required=False, **scenario)
    assert harness.client.login().status == 200
    return harness


@pytest.fixture
def device(document: Document) -> Harness:
    return signed_in(document)


def image(version: tuple[int, int, int, int] = (1, 0, 1, 0), **kw: Any) -> bytes:
    return mcuboot.build_image(b"\x5a" * 200, version=version, **kw)


def create(harness: Harness, data: bytes, target: str | None = STM32, **changes: Any):
    body: dict[str, Any] = {
        "filename": "zephyr.signed.bin",
        "size_bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    if target is not None:
        body["target"] = target
    body.update(changes)
    return harness.client.post("/firmware/uploads", body)


def send(harness: Harness, upload_id: str, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        chunk = data[offset : offset + UPLOAD_CHUNK_BYTES]
        assert (
            harness.client.put(f"/firmware/uploads/{upload_id}/data?offset={offset}", raw=chunk).status
            == 202
        )
        harness.advance(FLUSHED)
        offset += len(chunk)


def verified(harness: Harness, data: bytes, **changes: Any) -> dict:
    upload_id = create(harness, data, **changes).json["id"]
    send(harness, upload_id, data)
    harness.client.post(f"/firmware/uploads/{upload_id}/verify", {})
    harness.advance(VERIFIED)
    return harness.client.get(f"/firmware/uploads/{upload_id}").json


def ready(harness: Harness, data: bytes | None = None) -> str:
    body = verified(harness, image() if data is None else data)
    assert body["state"] == "ready", body
    return body["id"]


def install(harness: Harness, upload_id: str, downgrade: bool = False):
    return harness.client.post(
        "/system/updates", {"upload_id": upload_id, "acknowledge_downgrade": downgrade}
    )


def firmware(harness: Harness) -> dict:
    return harness.client.get("/system/firmware").json


def raw(harness: Harness, method: str, path: str) -> Any:
    return harness.app.handle(Request(method, path, {}, b""))


def back_after_swap(harness: Harness, seconds: float = SWAP) -> None:
    """Past the dark period, signed in again: the restart ended the session."""
    harness.advance(seconds + 0.1)
    assert harness.client.get("/system/status").status == 401, "sessions are RAM"
    assert harness.client.login().status == 200


def installed_unconfirmed(harness: Harness) -> str:
    upload_id = ready(harness)
    job = install(harness, upload_id).json["job_id"]
    harness.advance(TO_RESTART)
    assert harness.client.get("/system/status").status == 0, "the device is swapping"
    back_after_swap(harness)
    return job


# -- upload and verification ---------------------------------------------------


def test_an_upload_names_its_target_and_defaults_to_the_coprocessor(device: Harness) -> None:
    response = create(device, image())
    assert response.status == 201
    assert response.json["target"] == STM32
    device.client.delete(f"/firmware/uploads/{response.json['id']}")
    device.advance(1)
    legacy = device.client.post(
        "/firmware/uploads", {"filename": "c6.bin", "size_bytes": 600, "sha256": ESP_SHA}
    )
    assert legacy.status == 201
    assert legacy.json["target"] == "esp32c6"


def test_the_stm32_limit_is_slot_2_less_its_trailer_sector(device: Harness) -> None:
    limits = device.client.get("/capabilities").json["limits"]
    assert limits["system_upload_max_bytes"] == SYSTEM_UPLOAD_MAX_BYTES == 4_128_768
    too_big = create(device, b"", size_bytes=SYSTEM_UPLOAD_MAX_BYTES + 1, sha256=ESP_SHA)
    assert too_big.status == 413
    assert too_big.json["error"]["code"] == "payload_too_large"
    fits = create(device, b"", size_bytes=SYSTEM_UPLOAD_MAX_BYTES, sha256=ESP_SHA)
    assert fits.status == 201, "larger than the coprocessor's limit, and allowed"


def test_an_stm32_upload_does_not_need_room_on_littlefs(document: Document) -> None:
    harness = signed_in(document, storage_free_bytes=100)
    assert create(harness, image()).status == 201


def test_one_upload_exists_across_both_targets(device: Harness) -> None:
    create(device, image())
    esp = device.client.post(
        "/firmware/uploads",
        {"filename": "c6.bin", "size_bytes": 600, "sha256": ESP_SHA, "target": "esp32c6"},
    )
    assert esp.status == 409
    assert esp.json["error"]["code"] == "busy"


def test_a_coprocessor_upload_blocks_an_stm32_one(device: Harness) -> None:
    device.client.post(
        "/firmware/uploads", {"filename": "c6.bin", "size_bytes": 600, "sha256": ESP_SHA}
    )
    response = create(device, image())
    assert response.status == 409
    assert response.json["error"]["code"] == "busy"


def test_verification_parses_the_image_and_describes_it(device: Harness) -> None:
    body = verified(device, image(version=(2, 3, 4, 5)))
    assert body["state"] == "ready"
    assert body["error"] is None
    assert body["image"] == {
        "format": "mcuboot_image",
        "format_version": None,
        "target": STM32,
        "version": "2.3.4+5",
        "kind": "app",
        "partition_layout_id": None,
        "host_protocol": None,
        "signature_verified": None,
        "allowed_methods": ["ota"],
    }


def test_the_verify_result_knob_does_not_apply_to_an_stm32_image(document: Document) -> None:
    harness = signed_in(document, verify_result="invalid_image")
    assert verified(harness, image())["state"] == "ready"


def test_a_protected_tlv_area_is_accepted(device: Harness) -> None:
    protected = struct.pack("<HH", 0x50, 4) + b"\x01\x00\x00\x00"
    assert verified(device, image(protected=protected))["state"] == "ready"


#: Addresses written out rather than derived from mcuboot.py's constants, so a
#: broken constant cannot move a test's input along with the rule it tests.
APP_ENTRY = 0x0200_0401  # first Thumb address past the 0x400 header
HEADER_ADDR = 0x0200_03FD  # inside the header: not code
WINDOW_END = 0x0240_0001  # just past the 4 MiB window
SRAM_TOP = 0x200C_0000


def _without_sha_tlv() -> bytes:
    """The SHA-256 entry retyped, so the walk finds no digest."""
    data = image()
    at = len(data) - 36
    assert struct.unpack_from("<HH", data, at) == (mcuboot.TLV_SHA256, 32)
    return data[:at] + struct.pack("<H", 0x7F) + data[at + 2 :]


def _vectors(stack: int, reset: int) -> bytes:
    return image(stack=stack, reset=reset)


def _body_only(prot_size: int = 0) -> bytes:
    """Header and body with no TLV area at all; a protected area announced
    by `prot_size` but absent."""
    data = bytearray(image())
    header = struct.unpack_from("<IIHHI", data, 0)
    body_end = header[2] + header[4]
    struct.pack_into("<H", data, 10, prot_size)
    return bytes(data[:body_end])


def _protected_size_mismatch() -> bytes:
    """A protected TLV area whose own length disagrees with the header."""
    protected = struct.pack("<HH", 0x50, 4) + b"\x01\x00\x00\x00"
    data = bytearray(image(protected=protected))
    header = struct.unpack_from("<IIHHI", data, 0)
    struct.pack_into("<H", data, header[2] + header[4] + 2, 16)
    return bytes(data)


def _tlv_magic(magic: int) -> bytes:
    data = bytearray(image())
    at = len(data) - 40
    assert struct.unpack_from("<H", data, at)[0] == mcuboot.TLV_INFO_MAGIC
    struct.pack_into("<H", data, at, magic)
    return bytes(data)


FAILURES: list[tuple[str, Any, str, str]] = [
    ("not an image", lambda: b"\x00" * 1200, "invalid_image", "not an MCUboot image"),
    ("header size of another build", lambda: image(header_size=0x200), "unsupported_target", "header"),
    ("encrypted", lambda: image(flags=0x04), "invalid_image", "flags"),
    ("RAM load", lambda: image(flags=0x20), "invalid_image", "flags"),
    ("non-bootable", lambda: image(flags=0x10), "invalid_image", "flags"),
    ("compressed", lambda: image(flags=0x200), "invalid_image", "flags"),
    ("PIC", lambda: image(flags=0x01), "invalid_image", "flags"),
    ("protected area missing", lambda: _body_only(prot_size=8), "invalid_image", "protected TLV area lies past"),
    ("protected area of another size", _protected_size_mismatch, "invalid_image", "protected TLV area does not match"),
    ("no TLV area", _body_only, "invalid_image", "TLV area lies past"),
    ("TLV area magic wrong", lambda: _tlv_magic(0x6908), "invalid_image", "TLV area is missing"),
    ("junk after the TLV area", lambda: image() + b"\x00", "invalid_image", "does not end"),
    ("cut short", lambda: image()[:-1], "invalid_image", "does not end"),
    ("a TLV entry cut off", lambda: image(extra_tlvs=b"\x00\x00"), "invalid_image", "cut off"),
    ("a TLV entry past the area", lambda: image(extra_tlvs=struct.pack("<HH", 0x50, 100)), "invalid_image", "runs past"),
    ("digest wrong", lambda: image(corrupt_hash=True), "invalid_image", "does not match its contents"),
    ("no digest", _without_sha_tlv, "invalid_image", "no SHA-256"),
    ("a digest entry of the wrong size", lambda: image(extra_tlvs=struct.pack("<HH", 0x10, 4) + b"\x00" * 4), "invalid_image", "wrong length"),
    ("no room for a vector table", lambda: mcuboot.build_image(b"\x01\x02\x03\x04", vectors=False), "invalid_image", "too short"),
    ("stack below SRAM", lambda: _vectors(0x1000_0000, APP_ENTRY), "unsupported_target", "vector"),
    ("stack at the start of SRAM", lambda: _vectors(0x2000_0000, APP_ENTRY), "unsupported_target", "vector"),
    ("stack past SRAM", lambda: _vectors(SRAM_TOP + 4, APP_ENTRY), "unsupported_target", "vector"),
    ("stack at the start of PSRAM", lambda: _vectors(0x7000_0000, APP_ENTRY), "unsupported_target", "vector"),
    ("stack past PSRAM", lambda: _vectors(0x7080_0004, APP_ENTRY), "unsupported_target", "vector"),
    ("reset not Thumb", lambda: _vectors(SRAM_TOP, APP_ENTRY - 1), "unsupported_target", "vector"),
    ("reset in the header", lambda: _vectors(SRAM_TOP, HEADER_ADDR), "unsupported_target", "vector"),
    ("reset past the window", lambda: _vectors(SRAM_TOP, WINDOW_END), "unsupported_target", "vector"),
    ("reset in internal flash", lambda: _vectors(SRAM_TOP, 0x0800_0401), "unsupported_target", "vector"),
]


@pytest.mark.parametrize("name,make,code,words", FAILURES, ids=[f[0] for f in FAILURES])
def test_a_bad_image_fails_verification_with_its_code(
    device: Harness, name: str, make: Any, code: str, words: str
) -> None:
    data = make()
    upload_id = create(device, data).json["id"]
    send(device, upload_id, data)
    job = device.client.post(f"/firmware/uploads/{upload_id}/verify", {}).json["job_id"]
    device.advance(VERIFIED)
    body = device.client.get(f"/firmware/uploads/{upload_id}").json
    assert body["state"] == "failed", name
    assert body["error"]["code"] == code
    assert words in body["error"]["message"]
    assert body["image"] is None
    assert device.client.get(f"/jobs/{job}").json["error"]["code"] == code


def test_the_vector_table_bounds_that_are_allowed(device: Harness) -> None:
    for stack, reset in (
        (SRAM_TOP, APP_ENTRY),
        (0x2000_0004, 0x023F_FFFF),
        (0x7080_0000, 0x0200_0601),
        (0x7000_0004, APP_ENTRY),
    ):
        assert mcuboot.check(_vectors(stack, reset)).version == "1.0.1+0"


def test_an_unrelated_tlv_after_the_digest_is_accepted(device: Harness) -> None:
    extra = struct.pack("<HH", 0x50, 8) + b"\x00" * 8
    assert verified(device, image(extra_tlvs=extra))["state"] == "ready"


def test_the_install_job_parks_in_rebooting_and_never_succeeds(device: Harness) -> None:
    """Invisible through the API, because the restart answers first; read in the
    job store just before it. A job that reached `succeeded` would tell a client
    polling at the wrong moment that the update is done."""
    job_id = install(device, ready(device)).json["job_id"]
    device.advance(TO_RESTART + 1)  # settles the jobs, no request yet
    job = device.state.jobs.get(job_id)
    assert job.state == "running"
    assert job.phase == "rebooting"
    assert job.cancellable is False
    assert device.state.system.reboot_due is True


def test_a_declared_digest_that_does_not_match_outranks_the_structure(device: Harness) -> None:
    """Damage in transfer explains every other error too."""
    data = image(header_size=0x200)
    body = verified(device, data, sha256="1" * 64)
    assert body["error"]["code"] == "invalid_image"
    assert "declared" in body["error"]["message"]


def test_a_coprocessor_install_refuses_an_stm32_upload(device: Harness) -> None:
    upload_id = ready(device)
    response = device.client.post(
        "/coprocessor/updates",
        {"upload_id": upload_id, "method": "uart", "acknowledge_recovery": True},
    )
    assert response.status == 422
    assert response.json["error"]["code"] == "unsupported_target"


def test_a_reboot_keeps_a_received_stm32_upload(device: Harness) -> None:
    data = image()
    upload_id = ready(device, data)
    device.app.reboot()
    assert device.client.login().status == 200
    body = device.client.get(f"/firmware/uploads/{upload_id}").json
    assert body["state"] == "ready"
    assert install(device, upload_id).status == 202, "the bytes are still in slot 2"


# -- the running image -----------------------------------------------------------


def test_a_fresh_device_runs_a_confirmed_image(device: Harness) -> None:
    body = firmware(device)
    assert body["running"]["version"] == "1.0.0+0"
    assert body["running"]["confirmed"] is True
    assert len(body["running"]["image_hash"]) == 64
    assert body["confirm_remaining_seconds"] is None
    assert body["swap_pending"] is False
    assert body["update"] == {"available": True, "reason": None}
    assert body["last_update"] is None
    assert device.client.get("/system/status").json["firmware_version"] == "1.0.0+0"
    features = device.client.get("/capabilities").json["features"]
    assert features["stm32_update"] == {"available": True, "reason": None}


def test_the_running_version_is_a_scenario_knob(document: Document) -> None:
    harness = signed_in(document, system_version="3.1.4+1")
    assert firmware(harness)["running"]["version"] == "3.1.4+1"
    assert harness.client.get("/system/status").json["firmware_version"] == "3.1.4+1"


def test_an_unconfirmed_image_refuses_an_stm32_upload_but_not_a_coprocessor_one(
    document: Document,
) -> None:
    harness = signed_in(document, system_confirmed=False)
    body = firmware(harness)
    assert body["running"]["confirmed"] is False
    assert body["confirm_remaining_seconds"] == 1200
    assert body["update"] == {"available": False, "reason": "firmware_unconfirmed"}
    assert harness.client.get("/capabilities").json["features"]["stm32_update"] == body["update"]
    response = create(harness, image())
    assert response.status == 409
    assert response.json["error"]["code"] == "invalid_state"
    assert "not confirmed" in response.json["error"]["message"]
    esp = harness.client.post(
        "/firmware/uploads", {"filename": "c6.bin", "size_bytes": 600, "sha256": ESP_SHA}
    )
    assert esp.status == 201


def test_an_image_unconfirmed_from_the_start_confirms_itself(document: Document) -> None:
    harness = signed_in(document, system_confirmed=False, system_confirm_seconds=30)
    harness.advance(29.5)
    assert firmware(harness)["confirm_remaining_seconds"] == 0
    harness.advance(0.5)
    body = firmware(harness)
    assert body["running"]["confirmed"] is True
    assert body["last_update"] is None, "no install recorded one"


def test_an_image_unconfirmed_from_the_start_restarts_its_countdown_on_reboot(
    document: Document,
) -> None:
    harness = signed_in(document, system_confirmed=False, system_confirm_seconds=30)
    harness.advance(20)
    harness.app.reboot()
    assert harness.client.login().status == 200
    assert firmware(harness)["confirm_remaining_seconds"] == 30


# -- the install -------------------------------------------------------------------


def test_an_install_walks_its_phases_and_restarts_the_device(device: Harness) -> None:
    upload_id = ready(device)
    before = device.client.get("/system/status").json
    accepted = install(device, upload_id)
    assert accepted.status == 202
    assert accepted.json["resource_url"] == "/api/v1/system/firmware"
    job_id = accepted.json["job_id"]

    seen = []
    for _ in range(int(TO_RESTART / 0.25) - 1):
        job = device.client.get(f"/jobs/{job_id}").json
        seen.append((job["state"], job["phase"], job["cancellable"]))
        device.advance(0.25)
    phases = [p for _, p, _ in seen]
    assert phases[0] == "queued"
    assert [p for i, p in enumerate(phases) if i == 0 or phases[i - 1] != p] == [
        "queued",
        "preparing",
        "requesting",
        "rebooting",
    ]
    assert all(state in ("queued", "running") for state, _, _ in seen)
    assert {c for _, p, c in seen if p in ("queued", "preparing")} == {True}
    assert {c for _, p, c in seen if p in ("requesting", "rebooting")} == {False}
    assert device.client.get(f"/jobs/{job_id}").json["resource_url"] == "/api/v1/system/firmware"

    device.advance(0.5)
    assert raw(device, "GET", "/api/v1/auth/state").status == 0, "no answer during the swap"
    assert raw(device, "GET", "/").status == 0, "not even the web interface"
    state = raw(device, "GET", "/__mock/state").json
    assert state["unreachable_for_ms"] > 0, "the control plane still answers"

    back_after_swap(device)
    after = device.client.get("/system/status").json
    assert after["boot_id"] != before["boot_id"]
    assert int(after["uptime_ms"]) < 1000
    assert after["firmware_version"] == "1.0.1+0"
    assert device.client.get(f"/jobs/{job_id}").status == 404, "jobs are RAM"
    assert device.client.get(f"/firmware/uploads/{upload_id}").status == 404, "slot 2 was swapped"
    body = firmware(device)
    assert body["running"]["version"] == "1.0.1+0"
    assert body["running"]["confirmed"] is False
    assert body["running"]["image_hash"] == mcuboot.check(image()).image_hash
    assert body["swap_pending"] is False
    assert body["confirm_remaining_seconds"] == 1199
    assert body["update"] == {"available": False, "reason": "firmware_unconfirmed"}
    assert body["last_update"] == {
        "job_id": job_id,
        "state": "awaiting_confirmation",
        "from_version": "1.0.0+0",
        "version": "1.0.1+0",
        "error": None,
    }


def test_the_swap_is_pending_while_the_device_says_it_is_rebooting(device: Harness) -> None:
    upload_id = ready(device)
    job_id = install(device, upload_id).json["job_id"]
    device.advance(QUEUE_MS * SECOND + PHASE + 0.5)
    assert device.client.get(f"/jobs/{job_id}").json["phase"] == "requesting"
    assert firmware(device)["swap_pending"] is False
    assert firmware(device)["update"] == {"available": False, "reason": "update_running"}
    device.advance(PHASE)
    assert device.client.get(f"/jobs/{job_id}").json["phase"] == "rebooting"
    assert firmware(device)["swap_pending"] is True


def test_the_new_image_confirms_itself_when_its_time_is_up(device: Harness) -> None:
    job_id = installed_unconfirmed(device)
    device.advance(600)
    assert firmware(device)["confirm_remaining_seconds"] == 599
    device.advance(599.5)
    assert firmware(device)["running"]["confirmed"] is False
    device.advance(0.5)
    body = firmware(device)
    assert body["running"] == {
        "version": "1.0.1+0",
        "image_hash": mcuboot.check(image()).image_hash,
        "confirmed": True,
    }
    assert body["confirm_remaining_seconds"] is None
    assert body["update"] == {"available": True, "reason": None}
    assert body["last_update"]["state"] == "succeeded"
    assert body["last_update"]["job_id"] == job_id
    assert body["last_update"]["error"] is None


def test_the_confirmation_time_is_a_scenario_knob(document: Document) -> None:
    harness = signed_in(document, system_confirm_seconds=60)
    installed_unconfirmed(harness)
    assert firmware(harness)["confirm_remaining_seconds"] == 59
    harness.advance(60)
    assert firmware(harness)["last_update"]["state"] == "succeeded"


def test_the_restart_happens_when_rebooting_ends_however_late_the_next_request(
    device: Harness,
) -> None:
    upload_id = ready(device)
    install(device, upload_id)
    device.advance(TO_RESTART + SWAP + 30)
    assert device.client.get("/system/status").status == 401, "already back: only the session is gone"
    device.client.login()
    body = firmware(device)
    assert body["running"]["version"] == "1.0.1+0"
    assert body["confirm_remaining_seconds"] == 1200 - 30


def test_a_restart_before_confirmation_rolls_back(device: Harness) -> None:
    job_id = installed_unconfirmed(device)
    device.advance(100)
    raw(device, "POST", "/__mock/reboot")
    assert device.client.get("/system/status").status == 0, "MCUboot swaps back"
    back_after_swap(device)
    body = firmware(device)
    assert body["running"]["version"] == "1.0.0+0"
    assert body["running"]["confirmed"] is True
    assert body["confirm_remaining_seconds"] is None
    assert body["last_update"]["job_id"] == job_id
    assert body["last_update"]["state"] == "rolled_back"
    assert body["last_update"]["from_version"] == "1.0.0+0"
    assert body["last_update"]["version"] == "1.0.1+0"
    assert body["last_update"]["error"]["code"] == "boot_changed"
    assert device.client.get("/system/status").json["firmware_version"] == "1.0.0+0"
    assert create(device, image()).status == 201, "slot 2 is free again"


def test_a_restart_after_confirmation_changes_nothing(device: Harness) -> None:
    installed_unconfirmed(device)
    device.advance(1200)
    raw(device, "POST", "/__mock/reboot")
    assert device.client.login().status == 200, "no swap: the device answers at once"
    body = firmware(device)
    assert body["running"]["version"] == "1.0.1+0"
    assert body["last_update"]["state"] == "succeeded"


def test_an_image_mcuboot_rejects_fails_and_the_old_firmware_runs(document: Document) -> None:
    harness = signed_in(document, system_swap_result="rejected")
    upload_id = ready(harness)
    job_id = install(harness, upload_id).json["job_id"]
    harness.advance(TO_RESTART)
    assert harness.client.get("/system/status").status == 0
    back_after_swap(harness)
    body = firmware(harness)
    assert body["running"]["version"] == "1.0.0+0"
    assert body["running"]["confirmed"] is True
    assert body["last_update"]["job_id"] == job_id
    assert body["last_update"]["state"] == "failed"
    assert body["last_update"]["error"]["code"] == "invalid_image"
    assert harness.client.get(f"/firmware/uploads/{upload_id}").status == 404


def test_an_image_that_hangs_fails_after_two_swaps(document: Document) -> None:
    harness = signed_in(document, system_swap_result="hangs")
    install(harness, ready(harness))
    harness.advance(TO_RESTART + SWAP + 1)
    assert harness.client.get("/system/status").status == 0, "still dark after one swap"
    back_after_swap(harness, SWAP - 1)
    body = firmware(harness)
    assert body["running"]["version"] == "1.0.0+0"
    assert body["last_update"]["state"] == "failed"
    assert body["last_update"]["error"]["code"] == "internal_error"


def test_the_swap_time_is_a_scenario_knob(document: Document) -> None:
    harness = signed_in(document, system_swap_ms=40_000)
    install(harness, ready(harness))
    harness.advance(TO_RESTART + 39)
    assert harness.client.get("/system/status").status == 0
    back_after_swap(harness, 1)


def test_the_phase_times_are_scenario_knobs(document: Document) -> None:
    harness = signed_in(document, system_phase_ms=100, system_reboot_delay_ms=100)
    install(harness, ready(harness))
    harness.advance(QUEUE_MS * SECOND + 0.3 + 0.05)
    assert harness.client.get("/system/status").status == 0


def test_an_unknown_swap_result_is_a_mock_error_not_a_device_answer(document: Document) -> None:
    harness = signed_in(document, system_swap_result="explodes")
    install(harness, ready(harness))
    harness.advance(TO_RESTART)
    with pytest.raises(ValueError):
        harness.app.handle(Request("GET", "/api/v1/system/status", {}, b""))


def test_a_restart_before_the_swap_was_requested_interrupts_the_install(device: Harness) -> None:
    upload_id = ready(device)
    job_id = install(device, upload_id).json["job_id"]
    device.advance(QUEUE_MS * SECOND + PHASE / 2)
    raw(device, "POST", "/__mock/reboot")
    assert device.client.login().status == 200, "no swap, no dark period"
    body = firmware(device)
    assert body["running"]["version"] == "1.0.0+0"
    assert body["last_update"]["job_id"] == job_id
    assert body["last_update"]["state"] == "interrupted"
    assert body["last_update"]["error"]["code"] == "boot_changed"
    assert "preparing" in body["last_update"]["error"]["message"]
    assert device.client.get(f"/firmware/uploads/{upload_id}").json["state"] == "ready"
    assert install(device, upload_id).status == 202, "the file is free to install again"


def test_a_restart_during_rebooting_performs_the_swap(device: Harness) -> None:
    upload_id = ready(device)
    install(device, upload_id)
    device.advance(QUEUE_MS * SECOND + 2 * PHASE + 0.5)
    assert firmware(device)["swap_pending"] is True
    raw(device, "POST", "/__mock/reboot")
    assert device.client.get("/system/status").status == 0
    back_after_swap(device)
    assert firmware(device)["last_update"]["state"] == "awaiting_confirmation"


def test_an_install_can_be_cancelled_before_it_requests_the_swap(device: Harness) -> None:
    upload_id = ready(device)
    job_id = install(device, upload_id).json["job_id"]
    device.advance(QUEUE_MS * SECOND + 0.5)
    assert device.client.post(f"/jobs/{job_id}/cancel", {}).status == 202
    assert device.client.get(f"/jobs/{job_id}").json["state"] == "cancelled"
    device.advance(TO_RESTART + SWAP)
    assert device.client.get("/system/status").status == 200, "no restart came"
    body = firmware(device)
    assert body["last_update"] is None
    assert body["swap_pending"] is False
    assert body["update"]["available"] is True
    assert device.client.delete(f"/firmware/uploads/{upload_id}").status == 202, "file released"


def test_an_install_that_requested_the_swap_cannot_be_cancelled(device: Harness) -> None:
    upload_id = ready(device)
    job_id = install(device, upload_id).json["job_id"]
    device.advance(QUEUE_MS * SECOND + PHASE + 0.5)
    response = device.client.post(f"/jobs/{job_id}/cancel", {})
    assert response.status == 409
    assert response.json["error"]["code"] == "invalid_state"


def test_an_installing_upload_cannot_be_deleted_or_replaced(device: Harness) -> None:
    upload_id = ready(device)
    install(device, upload_id)
    assert device.client.delete(f"/firmware/uploads/{upload_id}").status == 409
    assert create(device, image()).json["error"]["code"] == "busy"


def test_active_job_ids_include_the_install(device: Harness) -> None:
    job_id = install(device, ready(device)).json["job_id"]
    assert job_id in device.client.get("/system/status").json["active_job_ids"]


# -- refusals, in the contract's order ---------------------------------------------


def _code(response: Any) -> tuple[int, str]:
    return response.status, response.json["error"]["code"]


def test_an_unknown_upload_is_not_found(device: Harness) -> None:
    assert _code(install(device, "upload_ffff")) == (404, "not_found")


def test_a_coprocessor_upload_is_unsupported_target(device: Harness) -> None:
    upload_id = device.client.post(
        "/firmware/uploads", {"filename": "c6.bin", "size_bytes": 600, "sha256": ESP_SHA}
    ).json["id"]
    assert _code(install(device, upload_id)) == (422, "unsupported_target")


def test_a_second_install_is_busy(device: Harness) -> None:
    upload_id = ready(device)
    install(device, upload_id)
    assert _code(install(device, upload_id)) == (409, "busy")


def test_a_running_coprocessor_install_makes_it_busy(device: Harness) -> None:
    """Not reachable through the API with one upload at a time; the rule is
    still the contract's, so it is set up underneath."""
    upload_id = ready(device)
    job = device.state.jobs.create("coprocessor_update", [Step("writing", 60_000)])
    device.state.coprocessor._installing_job = job.id
    assert _code(install(device, upload_id)) == (409, "busy")


@pytest.mark.parametrize("state,refused", [
    ("applying", True),
    ("awaiting_confirmation", True),
    ("staged", False),
    ("committed", False),
])
def test_a_network_change_in_flight_makes_it_busy(device: Harness, state: str, refused: bool) -> None:
    upload_id = ready(device)
    revision = device.client.get("/network/config").json["revision"]
    device.client.post("/network/transactions", {"base_revision": revision, "config": VALID_CANDIDATE})
    device.state.network.transaction.state = state
    response = install(device, upload_id)
    if refused:
        assert _code(response) == (409, "busy")
    else:
        assert response.status == 202


def test_an_unconfirmed_image_refuses_a_second_update(device: Harness) -> None:
    upload_id = ready(device)
    device.state.system.confirmed = False
    response = install(device, upload_id)
    assert _code(response) == (409, "invalid_state")
    assert "not confirmed" in response.json["error"]["message"]


def test_an_upload_that_is_not_ready_is_invalid_state(device: Harness) -> None:
    upload_id = create(device, image()).json["id"]
    response = install(device, upload_id)
    assert _code(response) == (409, "invalid_state")
    assert "'receiving'" in response.json["error"]["message"]


@pytest.mark.parametrize("running,candidate,refused", [
    ("2.0.0+0", (1, 9, 9, 9), True),
    ("1.0.1+5", (1, 0, 1, 4), True),
    ("1.0.1+0", (1, 0, 1, 0), False),
    ("1.0.0+0", (1, 0, 0, 1), False),
    ("1.2.0+0", (1, 10, 0, 0), False),
])
def test_a_downgrade_needs_acknowledgement(
    document: Document, running: str, candidate: tuple[int, int, int, int], refused: bool
) -> None:
    harness = signed_in(document, system_version=running)
    upload_id = ready(harness, image(version=candidate))
    response = install(harness, upload_id)
    if refused:
        assert _code(response) == (422, "validation_failed")
        assert "acknowledge_downgrade" in response.json["error"]["message"]
        assert install(harness, upload_id, downgrade=True).status == 202
    else:
        assert response.status == 202


def test_the_refusals_come_in_the_contracts_order(device: Harness) -> None:
    # unsupported_target before busy
    esp = device.client.post(
        "/firmware/uploads", {"filename": "c6.bin", "size_bytes": 600, "sha256": ESP_SHA}
    ).json["id"]
    device.state.system.confirmed = False
    assert _code(install(device, esp)) == (422, "unsupported_target")
    device.state.system.confirmed = True
    device.client.delete(f"/firmware/uploads/{esp}")
    device.advance(1)

    # busy before invalid_state (unconfirmed)
    upload_id = ready(device)
    job = device.state.jobs.create("coprocessor_update", [Step("writing", 60_000)])
    device.state.coprocessor._installing_job = job.id
    device.state.system.confirmed = False
    assert _code(install(device, upload_id)) == (409, "busy")
    job.finish(device.app.clock.now_ms(), "succeeded")

    # unconfirmed before not ready
    device.state.firmware.upload.state = "receiving"
    assert "not confirmed" in install(device, upload_id).json["error"]["message"]
    device.state.system.confirmed = True

    # not ready before the downgrade
    device.state.system.running_version = "9.0.0+0"
    assert _code(install(device, upload_id)) == (409, "invalid_state")


def test_a_swap_pending_refuses_a_new_stm32_upload(device: Harness) -> None:
    upload_id = ready(device)
    install(device, upload_id)
    device.advance(QUEUE_MS * SECOND + 2 * PHASE + 0.5)
    device.state.firmware.upload = None  # as if the file were gone: slot 2 still is not free
    response = create(device, image())
    assert _code(response) == (409, "invalid_state")
    assert "swap is already requested" in response.json["error"]["message"]


def test_the_upload_refusal_comes_after_busy_and_before_the_size(document: Document) -> None:
    harness = signed_in(document, system_confirmed=False)
    assert _code(create(harness, b"", size_bytes=SYSTEM_UPLOAD_MAX_BYTES + 1, sha256=ESP_SHA)) == (
        409,
        "invalid_state",
    )
    harness.client.post(
        "/firmware/uploads", {"filename": "c6.bin", "size_bytes": 600, "sha256": ESP_SHA}
    )
    assert _code(create(harness, image())) == (409, "busy")


# -- the HTTP adapter while the device is dark ----------------------------------


def test_the_server_closes_the_connection_without_an_answer(document: Document) -> None:
    source = FrozenSource()
    app = MockApp(
        document=document, clock=Clock(source=source), scenario=Scenario(setup_required=False)
    )
    app.unreachable_until_ms = 60_000
    handler = type("_BoundHandler", (_Handler,), {"app": app, "lock": threading.Lock()})
    httpd = HTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{httpd.server_address[1]}"
    try:
        with pytest.raises((urllib.error.URLError, ConnectionError)) as caught:
            urllib.request.urlopen(f"{base}/api/v1/auth/state", timeout=5)
        assert not isinstance(caught.value, urllib.error.HTTPError), "no HTTP status at all"
        with urllib.request.urlopen(f"{base}/__mock/state", timeout=5) as response:
            assert response.status == 200
        source.value = 60_000
        with urllib.request.urlopen(f"{base}/api/v1/auth/state", timeout=5) as response:
            assert response.status == 200
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join(timeout=5)


def test_a_reset_ends_the_dark_period(device: Harness) -> None:
    install(device, ready(device))
    device.advance(TO_RESTART)
    assert raw(device, "GET", "/api/v1/auth/state").status == 0
    raw(device, "POST", "/__mock/reset")
    assert raw(device, "GET", "/api/v1/auth/state").status == 200


def test_the_image_builder_and_the_check_agree_with_imgtool_layout() -> None:
    data = image(version=(1, 2, 3, 4))
    header = struct.unpack_from("<IIHHIIBBHII", data, 0)
    assert header[0] == mcuboot.IMAGE_MAGIC
    assert header[2] == 0x400
    tlv_off = header[2] + header[4] + header[3]
    assert struct.unpack_from("<HH", data, tlv_off) == (mcuboot.TLV_INFO_MAGIC, 40)
    assert len(data) == tlv_off + 40
    assert mcuboot.version_key("1.2.3+4") == (1, 2, 3, 4)
    assert mcuboot.version_key("1.2") == (1, 2, 0, 0)
