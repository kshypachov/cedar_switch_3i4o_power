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
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from ..errors import ApiError, error
from .clock import Clock
from .constants import (
    FIRMWARE_DELETE_MS,
    FIRMWARE_VERIFY_MS,
    INSTALL_PHASE_MS,
    QUEUE_MS,
    UPLOAD_CHUNK_BYTES,
    UPLOAD_CHUNK_MS,
    UPLOAD_MAX_BYTES,
)
from .jobs import JobStore, Step
from .scenario import Scenario
from .util import deep_copy, detail


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

    def to_json(self) -> dict[str, object]:
        return {
            "id": self.id,
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

        def flushed() -> None:
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
        outcome = self._scenario.verify_result

        def verified() -> None:
            upload.active_job_id = None
            if outcome == "ok":
                upload.state = "ready"
                upload.image = _image_json()
                upload.error = None
            else:
                upload.state = "failed"
                upload.image = None
                upload.error = error(outcome, _VERIFY_MESSAGES[outcome])

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
            final_state="succeeded" if outcome == "ok" else "failed",
        )
        if outcome != "ok":
            job.error = error(outcome, _VERIFY_MESSAGES[outcome])
        upload.state = "verifying"
        upload.active_job_id = job.id
        return job.id

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

    def _settle(self, upload: Upload) -> None:
        if upload.active_job_id is not None:
            self._jobs.get(upload.active_job_id)


_VERIFY_MESSAGES = {
    "invalid_image": "The file is not a recognised ESP32-C6 application image",
    "unsupported_target": "The image targets a different chip",
    "incompatible_firmware": "The image does not match this device's partition layout",
}


def _image_json() -> dict[str, object]:
    """A raw `.bin`, which is the only format the first version accepts.

    `signature_verified` is null rather than false: a raw application image
    carries no signature, and false would claim a check was made and failed.
    `allowed_methods` lists `uart` alone, because OTA is not implemented.
    """
    return {
        "format": "raw_app",
        "format_version": None,
        "target": "esp32c6",
        "version": "1.4.2",
        "kind": "app",
        "partition_layout_id": "cedar-c6-ota-2x1536k",
        "host_protocol": "esp-hosted-mcu-2.0",
        "signature_verified": None,
        "allowed_methods": ["uart"],
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

    def __init__(
        self, clock: Clock, jobs: JobStore, scenario: Scenario, firmware: Firmware
    ) -> None:
        self._clock = clock
        self._jobs = jobs
        self._scenario = scenario
        self._firmware = firmware
        self.version = "1.4.1"
        self.generation = 3
        self.last_update: dict[str, Any] | None = None
        self._installing_job: str | None = None

    def status_json(self) -> dict[str, object]:
        job = self._jobs.get(self._installing_job or "")
        updating = job is not None and not job.is_terminal
        ready = self._scenario.coprocessor_state == "ready"
        return {
            "state": "updating" if updating else self._scenario.coprocessor_state,
            "chip": "esp32c6",
            "firmware_version": self.version if ready else None,
            "host_protocol": "esp-hosted-mcu-2.0" if ready else None,
            "partition_layout_id": "cedar-c6-ota-2x1536k" if ready else None,
            "transport_ready": ready and not updating,
            "uart_mode": "flashing" if updating else ("console" if ready else "unavailable"),
            "generation": self.generation,
            "ota": {"available": False, "reason": self.OTA_REASON},
            "uart_update": {
                "available": ready,
                "reason": None
                if ready
                else f"the coprocessor is {self._scenario.coprocessor_state}",
            },
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
        if self._scenario.coprocessor_state != "ready":
            raise error(
                "service_not_ready",
                f"The coprocessor is {self._scenario.coprocessor_state}",
            )
        upload = self._firmware.find(body["upload_id"])
        if upload.state != "ready":
            raise error("invalid_state", f"The upload is {upload.state!r}, not 'ready'")
        job = self._jobs.get(self._installing_job or "")
        if job is not None and not job.is_terminal:
            raise error("busy", "A coprocessor update is already running")
        if upload.image and "recovery_bundle" == upload.image.get("kind"):
            if not body["acknowledge_recovery"]:
                raise error("validation_failed", "A recovery bundle needs acknowledge_recovery")

        failed = self._scenario.install_result != "ok"
        steps = [Step("queued", QUEUE_MS, state="queued")]
        for phase in self.INSTALL_PHASES:
            if phase == "writing":
                steps.append(Step(phase, INSTALL_PHASE_MS * 3, "bytes", upload.size_bytes))
            elif phase == "verifying":
                steps.append(Step(phase, INSTALL_PHASE_MS, "bytes", upload.size_bytes))
            elif phase == "health_check" and failed:
                break
            else:
                steps.append(Step(phase, INSTALL_PHASE_MS))

        def done() -> None:
            upload.installing = False
            self.generation += 1
            if failed:
                self.last_update = {
                    "job_id": new_job.id,
                    "state": "failed",
                    "method": "uart",
                    "version": None,
                    "recovery_required": True,
                    "error": detail(cause),
                }
            else:
                self.version = (upload.image or {}).get("version", self.version)
                self.last_update = {
                    "job_id": new_job.id,
                    "state": "succeeded",
                    "method": "uart",
                    "version": self.version,
                    "recovery_required": False,
                    "error": None,
                }

        cause = error(
            "internal_error",
            "The coprocessor did not come back after the write; recovery is required",
        )
        steps[-1] = Step(
            steps[-1].phase,
            steps[-1].duration_ms,
            steps[-1].unit,
            steps[-1].total,
            on_done=done,
        )
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
