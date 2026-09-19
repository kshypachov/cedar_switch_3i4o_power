# SPDX-License-Identifier: Apache-2.0
"""The STM32 update: the install job, MCUboot's swap across the restart, and the
new firmware confirming itself (api-contract.md, "Обновление STM32").

The part a static mock would get wrong is that the install ends *after* the job
does. The job's last phase is `rebooting`; the device then restarts, MCUboot
swaps the slots for tens of seconds while nothing answers, and the outcome is
decided on the other side: the new firmware runs unconfirmed and confirms
itself after a while, or a reset before that brings the old one back. So:

- **The job parks in `rebooting`** and never reaches `succeeded`; the restart
  removes it (a poll after it is 404, as for the coprocessor), and the outcome
  lives in `SystemFirmware.last_update`, which the update journal carries across.
- **The restart is real.** When `rebooting` ends, the next request finds a new
  boot: new `boot_id`, sessions gone, and for `system_swap_ms` no answer at all
  (the HTTP adapter closes the connection without a response), because a
  frontend that never saw the device vanish would treat the swap as a hang.
- **Any restart while a swap is pending performs it**, and **any restart while
  the new firmware is unconfirmed reverts it** - including `POST /__mock/reboot`,
  which is how a test cuts the power.
- **Slot 2 belongs to the swap.** After a swap (or MCUboot rejecting the image)
  the staged upload no longer describes what is in the slot, so it is gone.
"""

from __future__ import annotations

import hashlib
from typing import TYPE_CHECKING, Any

from ..errors import error
from .clock import Clock
from .constants import QUEUE_MS
from .jobs import Job, JobStore, Step
from .mcuboot import version_key
from .scenario import Scenario
from .util import deep_copy, detail

if TYPE_CHECKING:
    from .firmware import Coprocessor, Firmware
    from .network import Network

#: A network transaction in these states would be torn down by the restart.
_NETWORK_BUSY = frozenset({"applying", "awaiting_confirmation"})


class System:
    """The running STM32 image, its confirmation, and the install."""

    PHASES = ("preparing", "requesting", "rebooting")
    #: From this phase on the swap may already be requested in slot 2's trailer:
    #: the job can no longer be cancelled.
    POINT_OF_NO_RETURN = "requesting"

    def __init__(
        self,
        clock: Clock,
        jobs: JobStore,
        scenario: Scenario,
        firmware: Firmware,
        coprocessor: Coprocessor,
        network: Network,
    ) -> None:
        self._clock = clock
        self._jobs = jobs
        self._scenario = scenario
        self._firmware = firmware
        self._coprocessor = coprocessor
        self._network = network
        self.running_version = scenario.system_version
        self.running_hash = _hash_of(scenario.system_version)
        self.confirmed = bool(scenario.system_confirmed)
        self.confirm_deadline_ms: int | None = (
            None if self.confirmed else clock.now_ms() + scenario.system_confirm_seconds * 1000
        )
        self.swap_pending = False
        #: The stored coredump, kept across restarts like the device's flash
        #: partition until cleared; see record_crash().
        self.coredump: bytes | None = _coredump(0) if scenario.coredump_stored else None
        #: The install the journal records: job, upload, and both images.
        self.pending: dict[str, Any] | None = None
        #: Clock time at which the device restarts for the swap, once due.
        self.reboot_at_ms: int | None = None
        self.reboot_due = False
        self.last_update: dict[str, Any] | None = None
        self._job_id: str | None = None

    # -- reading ---------------------------------------------------------

    def updating(self) -> bool:
        job = self._jobs.get(self._job_id or "")
        return job is not None and not job.is_terminal

    def update_availability(self) -> tuple[bool, str | None]:
        """What `SystemFirmware.update` and `features.stm32_update` say.

        DECISION: the reason strings `firmware_unconfirmed` and `update_running`;
        the contract names none."""
        if not self.confirmed:
            return False, "firmware_unconfirmed"
        if self.updating():
            return False, "update_running"
        return True, None

    def upload_blocked(self) -> str | None:
        """Why a `stm32u585` upload may not start now, or None."""
        if self.swap_pending:
            return "A swap is already requested; slot 2 is not free until the device restarts"
        if not self.confirmed:
            return (
                "The running firmware is not confirmed yet; slot 2 holds the previous "
                "firmware until it is"
            )
        return None

    def confirm_remaining_seconds(self) -> int | None:
        if self.confirmed or self.confirm_deadline_ms is None:
            return None
        # Rounded down, like the network transaction's remaining_seconds.
        return max(0, (self.confirm_deadline_ms - self._clock.now_ms()) // 1000)

    def status_json(self) -> dict[str, object]:
        available, reason = self.update_availability()
        return {
            "running": {
                "version": self.running_version,
                "image_hash": self.running_hash,
                "confirmed": self.confirmed,
            },
            "confirm_remaining_seconds": self.confirm_remaining_seconds(),
            "swap_pending": self.swap_pending,
            "update": {"available": available, "reason": reason},
            "last_update": deep_copy(self.last_update),
        }

    # -- coredump --------------------------------------------------------

    def coredump_json(self) -> dict[str, object]:
        dump = self.coredump
        if dump is None:
            return {"coredump": None}
        reason = int.from_bytes(dump[8:12], "little")
        return {
            "coredump": {
                "size_bytes": len(dump),
                "reason": _REASONS.get(reason, "cpu_exception" if reason >= 16 else "other"),
                "reason_code": reason,
            }
        }

    def record_crash(self, reason: int) -> None:
        """What the fatal handler stores before the reset: a coredump."""
        self.coredump = _coredump(reason)

    # -- time ------------------------------------------------------------

    def settle(self) -> None:
        """The new firmware confirms itself once it has run long enough."""
        if self.confirmed or self.confirm_deadline_ms is None:
            return
        if self._clock.now_ms() < self.confirm_deadline_ms:
            return
        self.confirmed = True
        self.confirm_deadline_ms = None
        if self.last_update is not None and self.last_update["state"] == "awaiting_confirmation":
            self.last_update["state"] = "succeeded"
        self.pending = None

    # -- the install -----------------------------------------------------

    def start_update(self, body: dict[str, Any]) -> str:
        upload = self._firmware.find(body["upload_id"])
        if upload.target != "stm32u585":
            raise error(
                "unsupported_target",
                "This upload is a coprocessor image; install it with POST /coprocessor/updates",
            )
        if self.updating() or self._coprocessor.updating():
            raise error("busy", "A firmware update is already running")
        transaction = self._network.transaction
        if transaction is not None and transaction.state in _NETWORK_BUSY:
            raise error("busy", "A network change is being applied; the restart would undo it")
        if not self.confirmed:
            raise error(
                "invalid_state",
                "The running firmware is not confirmed yet; a second update would lose "
                "the way back",
            )
        if upload.state != "ready":
            raise error("invalid_state", f"The upload is {upload.state!r}, not 'ready'")
        image = upload.image or {}
        version = str(image.get("version"))
        if version_key(version) < version_key(self.running_version) and not body[
            "acknowledge_downgrade"
        ]:
            raise error(
                "validation_failed",
                f"The image ({version}) is older than the running firmware "
                f"({self.running_version}); set acknowledge_downgrade to true",
            )

        scenario = self._scenario
        now = self._clock.now_ms()
        self.pending = {
            "upload_id": upload.id,
            "from_version": self.running_version,
            "from_hash": self.running_hash,
            "version": version,
            "hash": upload.image_hash,
        }

        def requested() -> None:
            self.swap_pending = True

        def due() -> None:
            self.reboot_due = True

        steps = [
            Step("queued", QUEUE_MS, state="queued"),
            Step("preparing", scenario.system_phase_ms),
            Step(
                "requesting", scenario.system_phase_ms, on_done=requested, cancellable=False
            ),
            Step("rebooting", scenario.system_reboot_delay_ms, on_done=due, cancellable=False),
        ]
        job = self._jobs.create(
            "system_update",
            steps,
            resource_url="/api/v1/system/firmware",
            park_state="running",
        )
        self.pending["job_id"] = job.id
        self.reboot_at_ms = now + sum(step.duration_ms for step in steps)
        upload.installing = True
        self._job_id = job.id
        return job.id

    def cancelled(self, job: Job) -> None:
        """Cancelled before `requesting`: nothing reached slot 2's trailer."""
        if job.id != self._job_id:
            return
        if self._firmware.upload is not None:
            self._firmware.upload.installing = False
        self.pending = None
        self.reboot_at_ms = None

    # -- the restart -----------------------------------------------------

    def survive_reboot(self, old: System, at_ms: int) -> tuple[int, str | None]:
        """Carry the journal and the slots across a restart at `at_ms`.

        Returns how long the device answers nothing (the swap), and the upload
        the swap consumed, if any."""
        scenario = self._scenario
        swap_ms = int(scenario.system_swap_ms)
        self.running_version = old.running_version
        self.running_hash = old.running_hash
        self.confirmed = old.confirmed
        self.confirm_deadline_ms = old.confirm_deadline_ms
        self.last_update = deep_copy(old.last_update)
        self.coredump = old.coredump
        pending = old.pending
        job = old._jobs.get(old._job_id or "")

        if old.swap_pending and pending is not None:
            result = scenario.system_swap_result
            if result == "ok":
                back = at_ms + swap_ms
                self.running_version = pending["version"]
                self.running_hash = pending["hash"]
                self.confirmed = False
                self.confirm_deadline_ms = back + scenario.system_confirm_seconds * 1000
                self.pending = pending
                self.last_update = _summary(pending, "awaiting_confirmation")
                return swap_ms, pending["upload_id"]
            if result == "rejected":
                self.last_update = _summary(
                    pending,
                    "failed",
                    "invalid_image",
                    "MCUboot did not accept the image in slot 2; the previous firmware runs",
                )
                return swap_ms, pending["upload_id"]
            if result == "hangs":
                # Swapped in, hung, reset by the watchdog, swapped back.
                self.last_update = _summary(
                    pending,
                    "failed",
                    "internal_error",
                    "The new firmware did not start; the watchdog reset the device and "
                    "MCUboot restored the previous firmware",
                )
                return 2 * swap_ms, pending["upload_id"]
            raise ValueError(f"unknown system_swap_result {result!r}")

        if not old.confirmed and pending is not None:
            # Reset before confirmation: MCUboot swaps the previous firmware back.
            self.running_version = pending["from_version"]
            self.running_hash = pending["from_hash"]
            self.confirmed = True
            self.confirm_deadline_ms = None
            self.last_update = _summary(
                pending,
                "rolled_back",
                "boot_changed",
                "The device restarted before the new firmware was confirmed; MCUboot "
                "restored the previous firmware",
            )
            return swap_ms, None

        if not old.confirmed and self.confirm_deadline_ms is not None:
            # An image unconfirmed from the start (scenario) with no journal: the
            # mock knows no previous image to restore, so the countdown restarts.
            self.confirm_deadline_ms = at_ms + scenario.system_confirm_seconds * 1000

        if job is not None and not job.is_terminal and pending is not None:
            self.last_update = _summary(
                pending,
                "interrupted",
                "boot_changed",
                f"The device restarted during the {job.phase} phase, before the swap was "
                "requested; nothing changed",
            )
        return 0, None


#: The kernel's K_ERR_* codes by name, as getCoredump reports them.
_REASONS = {
    0: "cpu_exception",
    1: "spurious_irq",
    2: "stack_check_fail",
    3: "kernel_oops",
    4: "kernel_panic",
}


def _coredump(reason: int) -> bytes:
    """A stand-in for Zephyr's binary coredump: the real header (`ZE`, version 2,
    ARM Cortex-M target 3, 32-bit pointers, `reason`) and filler for the blocks."""
    header = b"ZE" + (2).to_bytes(2, "little") + (3).to_bytes(2, "little") + bytes([5, 0])
    return header + reason.to_bytes(4, "little") + bytes(range(256)) * 32


def _hash_of(version: str) -> str:
    """A stable stand-in TLV hash for the image the scenario says is running."""
    return hashlib.sha256(f"cedar-mock-stm32-{version}".encode()).hexdigest()


def _summary(
    pending: dict[str, Any], state: str, code: str | None = None, message: str | None = None
) -> dict[str, Any]:
    return {
        "job_id": pending["job_id"],
        "state": state,
        "from_version": pending["from_version"],
        "version": pending["version"],
        "error": None if code is None else detail(error(code, message or "")),
    }
