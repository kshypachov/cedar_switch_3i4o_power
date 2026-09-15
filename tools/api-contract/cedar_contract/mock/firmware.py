# SPDX-License-Identifier: Apache-2.0
"""Upload, verification, and the UART install.

The one behaviour here worth more than a static answer is the offset. The
contract makes `received_bytes` the next acceptable offset *after a crash*,
which means it may only advance once the chunk is on storage. So a chunk request
returns a job and the counter moves when that job completes, never when the
bytes arrive. A mock that incremented on arrival would let the frontend build a
resume path that skips a chunk the device never wrote.

The second is the OTA refusal. `method="ota"` is in the schema for forward
compatibility and rejected with `503 capability_unavailable`, and both this
module and capabilities report `available=false` with `reason="not_implemented"`
— the owner's decision, and the reason the UI must show instead of a disabled
placeholder switch.

P6 made the file a whole flash image (`raw_full_flash`, the owner's decision):
bootloader, partition table, blank otadata and application, written from 0x0.
That makes every image a `recovery_bundle` that needs `acknowledge_recovery`,
and it makes an install the way an empty or broken coprocessor gets firmware,
so the coprocessor's state no longer gates it — only who owns the UART does.

The STM32 update stage gave an upload a `target`. A `stm32u585` upload goes
straight into MCUboot's slot 2 on the device, so the mock keeps its bytes and
verification parses them as an MCUboot image (mcuboot.py) instead of reading a
scenario knob; the install is `system.py`'s. One upload exists at a time, of
either target.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field, replace
from typing import TYPE_CHECKING, Any

from ..errors import ApiError, error
from . import mcuboot
from .clock import Clock
from .constants import (
    FIRMWARE_DELETE_MS,
    FIRMWARE_VERIFY_MS,
    INSTALL_PHASE_MS,
    QUEUE_MS,
    SYSTEM_UPLOAD_MAX_BYTES,
    UPLOAD_CHUNK_BYTES,
    UPLOAD_CHUNK_MS,
    UPLOAD_MAX_BYTES,
)
from .jobs import Job, JobStore, Step
from .scenario import Scenario
from .util import deep_copy, detail

if TYPE_CHECKING:
    from .system import System

STM32 = "stm32u585"
ESP32 = "esp32c6"

#: The one partition layout the device accepts: nvs, otadata, phy_init and two
#: application slots of 0x1c0000 (1792 KiB) on 4 MB flash.
LAYOUT_ID = "cedar-c6-ota-4m-2x1792k"
#: The device's profile value. It is not read from the image: a Wi-Fi and an OT
#: coprocessor build carry identical headers, and the compatibility profile that
#: would tell them apart is deferred by the owner.
HOST_PROTOCOL = "esp-hosted-mcu-3"
#: `app_desc.version` of the image the mock pretends was uploaded.
IMAGE_VERSION = "1"


def host_protocol_of(hosted_version: str) -> str:
    """`esp-hosted-mcu-<major>` of a version the C6 reports, e.g. "v3.0.6" → 3."""
    major = hosted_version.lstrip("vV").split(".")[0]
    return f"esp-hosted-mcu-{major}"


@dataclass
class Upload:
    id: str
    filename: str
    size_bytes: int
    sha256: str
    received_bytes: int = 0
    state: str = "receiving"
    active_job_id: str | None = None
    image: dict[str, Any] | None = None
    error: ApiError | None = None
    installing: bool = False
    target: str = ESP32
    #: The bytes on storage, kept for a `stm32u585` upload only (verified by parsing).
    data: bytearray = field(default_factory=bytearray)
    #: The MCUboot TLV SHA-256 of a verified `stm32u585` image.
    image_hash: str | None = None

    def to_json(self) -> dict[str, object]:
        return {
            "id": self.id,
            "target": self.target,
            "filename": self.filename,
            "size_bytes": self.size_bytes,
            "received_bytes": self.received_bytes,
            "sha256": self.sha256,
            "state": self.state,
            "active_job_id": self.active_job_id,
            "image": deep_copy(self.image),
            "error": None if self.error is None else detail(self.error),
        }


class Firmware:
    """The staged file: create, receive chunks, verify, delete."""

    def __init__(self, clock: Clock, jobs: JobStore, scenario: Scenario) -> None:
        self._clock = clock
        self._jobs = jobs
        self._scenario = scenario
        self.upload: Upload | None = None
        self._counter = 0
        #: Set by the device state: what may keep slot 2 from taking an upload.
        self.system: System | None = None

    def find(self, upload_id: str) -> Upload:
        if self.upload is None or self.upload.id != upload_id:
            raise error("not_found", "No such upload")
        self._settle(self.upload)
        return self.upload

    def create(self, body: dict[str, Any]) -> Upload:
        if self.upload is not None:
            self._settle(self.upload)
            if self.upload.installing:
                raise error("busy", "An install is using the staged image")
            if self.upload.state != "failed":
                # "One active upload", and the contract forbids deleting a ready
                # package on the server's initiative before the update outcome
                # is recorded. So a new file needs the old one deleted first,
                # and only a failed upload may be replaced in place.
                raise error(
                    "busy",
                    f"Upload {self.upload.id} is {self.upload.state!r}; delete it first",
                )
        target = body.get("target", ESP32)
        if target == STM32:
            # DECISION: busy (another upload), then invalid_state (slot 2 not
            # free), then the size; the contract lists the three without an order.
            blocked = self.system.upload_blocked() if self.system is not None else None
            if blocked is not None:
                raise error("invalid_state", blocked)
            if body["size_bytes"] > SYSTEM_UPLOAD_MAX_BYTES:
                raise error(
                    "payload_too_large",
                    f"The image is larger than slot 2 can hold ({SYSTEM_UPLOAD_MAX_BYTES} bytes)",
                )
            # No space check: on the device only the metadata goes to LittleFS.
        else:
            if body["size_bytes"] > UPLOAD_MAX_BYTES:
                raise error(
                    "payload_too_large",
                    f"The image is larger than the {UPLOAD_MAX_BYTES} byte limit",
                )
            if body["size_bytes"] > self._scenario.storage_free_bytes:
                # Checked before the first byte and held for the whole upload, so
                # the refusal arrives up front rather than at a failed write.
                raise error("storage_full", "Not enough free space on the device for this image")
        self._counter += 1
        self.upload = Upload(
            id=f"upload_{self._counter:04x}",
            filename=body["filename"],
            size_bytes=body["size_bytes"],
            sha256=body["sha256"],
            target=target,
        )
        return self.upload

    def write_chunk(self, upload: Upload, offset: int, data: bytes) -> str:
        if upload.state != "receiving":
            raise error("invalid_state", f"An upload in state {upload.state!r} takes no more data")
        if upload.active_job_id is not None:
            raise error("busy", "The previous chunk is still being written")
        if offset != upload.received_bytes:
            raise error(
                "offset_mismatch",
                f"The next acceptable offset is {upload.received_bytes}",
            )
        if not data:
            raise error("validation_failed", "An empty chunk writes nothing")
        if len(data) > UPLOAD_CHUNK_BYTES:
            raise error("payload_too_large", f"A chunk may not exceed {UPLOAD_CHUNK_BYTES} bytes")
        if offset + len(data) > upload.size_bytes:
            raise error("validation_failed", "The chunk runs past the declared size")

        written = len(data)
        chunk = bytes(data)

        def flushed() -> None:
            if upload.target == STM32:
                upload.data += chunk
            upload.received_bytes += written
            upload.active_job_id = None

        job = self._jobs.create(
            "upload_chunk",
            [Step("writing", UPLOAD_CHUNK_MS, "bytes", written, on_done=flushed)],
            resource_url=f"/api/v1/firmware/uploads/{upload.id}",
        )
        upload.active_job_id = job.id
        return job.id

    def verify(self, upload: Upload) -> str:
        if upload.received_bytes != upload.size_bytes:
            raise error(
                "invalid_state",
                f"{upload.received_bytes} of {upload.size_bytes} bytes received",
            )
        if upload.state not in ("receiving", "failed"):
            raise error(
                "invalid_state", f"An upload in state {upload.state!r} is already verified"
            )
        if upload.active_job_id is not None:
            raise error("busy", "A chunk is still being written")
        image_hash: str | None = None
        if upload.target == STM32:
            code, message, image = _check_stm32(upload)
            ok = image is not None
            if image is not None:
                image_hash = image.image_hash
        else:
            code, message = _VERIFY_OUTCOMES.get(self._scenario.verify_result, (None, None))
            ok = self._scenario.verify_result == "ok"
            if not ok and code is None:
                raise ValueError(f"unknown verify_result {self._scenario.verify_result!r}")
            image = None

        def verified() -> None:
            upload.active_job_id = None
            if ok:
                upload.state = "ready"
                upload.image = _image_json() if image is None else _stm32_image_json(image)
                upload.image_hash = image_hash
                upload.error = None
            else:
                upload.state = "failed"
                upload.image = None
                upload.error = error(code, message)

        job = self._jobs.create(
            "firmware_verify",
            [
                Step("queued", QUEUE_MS, state="queued"),
                Step(
                    "verifying",
                    FIRMWARE_VERIFY_MS,
                    "bytes",
                    upload.size_bytes,
                    on_done=verified,
                ),
            ],
            resource_url=f"/api/v1/firmware/uploads/{upload.id}",
            final_state="succeeded" if ok else "failed",
        )
        if not ok:
            job.error = error(code, message)
        upload.state = "verifying"
        upload.active_job_id = job.id
        return job.id

    def verify_cancelled(self, job: Job) -> None:
        """A cancelled verification leaves the file as it was before: every byte
        received and nothing known about it, so it can be verified again.
        DECISION: `receiving`, not `failed` — nothing was found wrong."""
        upload = self.upload
        if upload is not None and upload.active_job_id == job.id:
            upload.active_job_id = None
            upload.state = "receiving"

    def delete(self, upload: Upload) -> str:
        if upload.installing:
            raise error("busy", "The staged image is in use by an install")

        def removed() -> None:
            self.upload = None

        job = self._jobs.create(
            "firmware_delete",
            [Step("deleting", FIRMWARE_DELETE_MS, on_done=removed)],
            resource_url=None,
        )
        upload.active_job_id = job.id
        return job.id

    def survive_reboot(self, old: Firmware) -> None:
        """The file and its metadata are on LittleFS and outlive a reboot; the
        jobs working on them do not. A chunk not yet flushed is not counted, a
        verification in progress starts over, and nothing is installing."""
        self._counter = old._counter
        upload = old.upload
        if upload is None:
            return
        old._settle(upload)
        if upload.state == "verifying":
            upload.state = "receiving"
        upload.active_job_id = None
        upload.installing = False
        self.upload = upload

    def _settle(self, upload: Upload) -> None:
        if upload.active_job_id is not None:
            self._jobs.get(upload.active_job_id)


#: `verify_result` knob → the code and message the device's check produces.
_VERIFY_OUTCOMES: dict[str, tuple[str, str]] = {
    "invalid_image": (
        "invalid_image",
        "The file is not a whole ESP32-C6 flash image: a checksum, digest or header is wrong",
    ),
    "bare_app": (
        "invalid_image",
        "The file is a bare application .bin; this device takes the merged file "
        "(idf.py merge-bin) that is written from 0x0",
    ),
    "unsupported_target": ("unsupported_target", "The image targets a different chip"),
    "incompatible_firmware": (
        "incompatible_firmware",
        "The image's partition table does not match the layout this device supports",
    ),
}


def _image_json() -> dict[str, object]:
    """A whole flash image, the only format the first version accepts.

    `kind` is `recovery_bundle` because writing it replaces the coprocessor's
    bootloader and partition table and erases its NVS. `signature_verified` is
    null rather than false: the file carries no signature, and false would claim
    a check was made and failed. `allowed_methods` lists `uart` alone, because
    OTA is not implemented.
    """
    return {
        "format": "raw_full_flash",
        "format_version": None,
        "target": "esp32c6",
        "version": IMAGE_VERSION,
        "kind": "recovery_bundle",
        "partition_layout_id": LAYOUT_ID,
        "host_protocol": HOST_PROTOCOL,
        "signature_verified": None,
        "allowed_methods": ["uart"],
    }


def _check_stm32(upload: Upload) -> tuple[str | None, str | None, mcuboot.McubootImage | None]:
    """The declared digest first: damage in transfer explains any structural
    error too, so it is the one reported (firmware-store's rule)."""
    if hashlib.sha256(bytes(upload.data)).hexdigest() != upload.sha256:
        return (
            "invalid_image",
            "The file's SHA-256 does not match the one declared when the upload was created",
            None,
        )
    try:
        return None, None, mcuboot.check(bytes(upload.data))
    except mcuboot.ImageRejected as rejected:
        return rejected.code, rejected.message, None


def _stm32_image_json(image: mcuboot.McubootImage) -> dict[str, object]:
    """An application signed by imgtool. `allowed_methods` is `ota`: installed
    over the network and swapped in by MCUboot. No layout or host protocol."""
    return {
        "format": "mcuboot_image",
        "format_version": None,
        "target": STM32,
        "version": image.version,
        "kind": "app",
        "partition_layout_id": None,
        "host_protocol": None,
        "signature_verified": None,
        "allowed_methods": ["ota"],
    }


class Coprocessor:
    """C6 status and the UART install job.

    `ota` reports `available=false` with `reason="not_implemented"` here and in
    capabilities, which is the owner's decision recorded in section 13 of the
    plan. The UI must show the reason; that is why the reason is a string from
    the server and not a flag the frontend interprets.
    """

    OTA_REASON = "not_implemented"

    #: The first version's UART phases. `activating` and `confirming` are absent
    #: on purpose: the contract says a successful `health_check` after a normal
    #: boot is the confirmation in the UART path.
    INSTALL_PHASES = (
        "preflight",
        "entering_bootloader",
        "begin",
        "writing",
        "verifying",
        "reconnecting",
        "health_check",
        "complete",
    )
    #: From this phase on the chip's flash is being erased or written: the job
    #: can no longer be cancelled, and an interruption leaves recovery to do.
    DESTRUCTIVE_PHASE = "begin"

    def __init__(
        self, clock: Clock, jobs: JobStore, scenario: Scenario, firmware: Firmware
    ) -> None:
        self._clock = clock
        self._jobs = jobs
        self._scenario = scenario
        self._firmware = firmware
        self.generation = 3
        self.last_update: dict[str, Any] | None = None
        self._installing_job: str | None = None

    def updating(self) -> bool:
        job = self._jobs.get(self._installing_job or "")
        return job is not None and not job.is_terminal

    def uart_mode(self) -> str:
        """Who owns the UART. On the device this is coprocessor-manager's, and it
        does not depend on whether the C6 answers over ESP-Hosted: board B's C6
        has no firmware, and its UART still belongs to the log console."""
        return "flashing" if self.updating() else self._scenario.uart_mode

    def uart_update_availability(self) -> tuple[bool, str | None]:
        """Whether an install could start now, and why not. It follows the
        UART's owner, never the coprocessor's state: an install is how a C6 with
        no firmware gets some."""
        mode = self.uart_mode()
        if mode == "console":
            return True, None
        return False, f"uart_{mode}"

    def status_json(self) -> dict[str, object]:
        updating = self.updating()
        ready = self._scenario.coprocessor_state == "ready"
        transport = ready and not updating
        available, reason = self.uart_update_availability()
        # What the C6 reports over ESP-Hosted, and only while it can report it.
        hosted = self._scenario.hosted_version if transport else None
        return {
            "state": "updating" if updating else self._scenario.coprocessor_state,
            "chip": "esp32c6",
            "firmware_version": hosted,
            "host_protocol": None if hosted is None else host_protocol_of(hosted),
            "partition_layout_id": LAYOUT_ID if ready else None,
            "transport_ready": transport,
            "uart_mode": self.uart_mode(),
            "generation": self.generation,
            "ota": {"available": False, "reason": self.OTA_REASON},
            "uart_update": {"available": available, "reason": reason},
            "last_update": deep_copy(self.last_update),
        }

    def start_update(self, body: dict[str, Any]) -> str:
        if body["method"] == "ota":
            # Reserved in the enum so adding it later needs no v2, and refused
            # with the code the contract names for a capability this build does
            # not have.
            raise error(
                "capability_unavailable",
                'ESP32 OTA is not implemented in this version; use method "uart"',
            )
        if self._scenario.install_transport != "ethernet":
            # The contract requires the install request itself to arrive over
            # Ethernet, and to refuse before any reset or erase.
            raise error(
                "ethernet_required",
                "A coprocessor update must be requested over Ethernet",
            )
        upload = self._firmware.find(body["upload_id"])
        if upload.target != ESP32:
            # DECISION: the contract names the refusal only for the other
            # direction (a coprocessor file sent to startSystemUpdate); the same
            # code, right after the upload is found.
            raise error(
                "unsupported_target",
                "This upload is an STM32 image; install it with POST /system/updates",
            )
        if self.updating():
            raise error("busy", "A coprocessor update is already running")
        if upload.state != "ready":
            raise error("invalid_state", f"The upload is {upload.state!r}, not 'ready'")
        if (upload.image or {}).get("kind") == "recovery_bundle" and not body[
            "acknowledge_recovery"
        ]:
            raise error(
                "validation_failed",
                "This image replaces the coprocessor's bootloader and partition table "
                "and erases its NVS; set acknowledge_recovery to true",
            )
        mode = self._scenario.uart_mode
        if mode == "usb_bridge":
            raise error("busy", "The coprocessor's UART is bridged to USB")
        if mode != "console":
            # DECISION: the UART cannot be handed to a flasher at all, which is a
            # capability missing now rather than a conflict that clears by itself.
            raise error("capability_unavailable", f"The coprocessor's UART is {mode}")

        failed = self._scenario.install_result != "ok"
        steps = [Step("queued", QUEUE_MS, state="queued")]
        destructive = False
        for phase in self.INSTALL_PHASES:
            destructive = destructive or phase == self.DESTRUCTIVE_PHASE
            if phase == "writing":
                step = Step(phase, INSTALL_PHASE_MS * 3, "bytes", upload.size_bytes)
            elif phase == "verifying":
                step = Step(phase, INSTALL_PHASE_MS, "bytes", upload.size_bytes)
            elif phase == "health_check" and failed:
                break
            else:
                step = Step(phase, INSTALL_PHASE_MS)
            steps.append(replace(step, cancellable=not destructive))

        def done() -> None:
            upload.installing = False
            self.generation += 1
            if failed:
                # The chip did not come back: it answers nothing over ESP-Hosted.
                self._scenario.coprocessor_state = "failed"
                self.last_update = {
                    "job_id": new_job.id,
                    "state": "failed",
                    "method": "uart",
                    "version": None,
                    "recovery_required": True,
                    "error": detail(cause),
                }
            else:
                # Written and confirmed: a C6 that had no firmware now has some.
                # The summary names the image's app_desc version; the status goes
                # on showing what the C6 itself reports over ESP-Hosted.
                self._scenario.coprocessor_state = "ready"
                self.last_update = {
                    "job_id": new_job.id,
                    "state": "succeeded",
                    "method": "uart",
                    "version": (upload.image or {}).get("version"),
                    "recovery_required": False,
                    "error": None,
                }

        cause = error(
            "internal_error",
            "The coprocessor did not come back after the write; recovery is required",
        )
        steps[-1] = replace(steps[-1], on_done=done)
        new_job = self._jobs.create(
            "coprocessor_update",
            steps,
            resource_url="/api/v1/coprocessor/status",
            final_state="failed" if failed else "succeeded",
        )
        if failed:
            new_job.error = cause
        upload.installing = True
        self._installing_job = new_job.id
        return new_job.id

    def cancelled(self, job: Job) -> None:
        """An install cancelled before its destructive phase touched nothing on
        the chip: the file is free again and `last_update` stays as it was."""
        if job.id == self._installing_job and self._firmware.upload is not None:
            self._firmware.upload.installing = False

    def survive_reboot(self, old: Coprocessor) -> None:
        """The update journal is on LittleFS. An install the reboot cut short
        becomes `interrupted` and nothing continues it; whether recovery is
        needed depends on whether the destructive phase had begun."""
        # Settle the install first: one that finished before the reboot writes
        # its outcome as it settles, and that outcome is what the journal holds.
        job = old._jobs.get(old._installing_job or "")
        self.last_update = deep_copy(old.last_update)
        if job is None or job.is_terminal:
            return
        phases = self.INSTALL_PHASES
        reached = job.phase in phases and phases.index(job.phase) >= phases.index(
            self.DESTRUCTIVE_PHASE
        )
        if reached:
            # Part of the chip's flash is erased or written: it boots nothing.
            self._scenario.coprocessor_state = "failed"
        self.last_update = {
            "job_id": job.id,
            "state": "interrupted",
            "method": "uart",
            "version": None,
            "recovery_required": reached,
            "error": detail(
                error(
                    "boot_changed",
                    f"The device restarted during the {job.phase} phase; "
                    "nothing continues until the install is requested again",
                )
            ),
        }
