# SPDX-License-Identifier: Apache-2.0
"""Jobs that take time, because a job that does not is untestable.

`api-contract.md` says a `202` means work was accepted, not that it succeeded,
and it tells the client to poll every 500-1000 ms. A mock that answered
`succeeded` on the first `GET /jobs/{id}` would let a frontend ship a polling
loop that has never once seen `queued`, `running`, a phase change, or progress
resetting to zero at the start of a new phase. So a job here is a timeline.

The model is four ideas:

- **A job is a list of steps**, each with a phase name, a duration, and
  optionally a progress unit and total. Progress belongs to the current phase
  and restarts at zero when the phase changes, which is what the contract says
  and the opposite of what a naive UI would draw.
- **State is derived from the clock, not pushed by a worker.** There is no
  thread. Every request settles the store first, so any number of pollers see
  one consistent progression and a test can step the clock instead of sleeping.
- **A job can park.** `network_apply` stops at `waiting_confirmation` and stays
  there until something confirms, rolls it back, or the timeout fires. Parking
  is not a terminal state and not a phase — it is the job waiting for the world.
- **Effects fire once, when the step that earned them completes.** An upload's
  `received_bytes` advances when the chunk job finishes, never when the request
  arrives, because the contract makes that value the next safe offset after a
  crash. Tying the effect to the step is what keeps that true for free.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable
from dataclasses import dataclass, field

from ..errors import ApiError, detail_dict
from .clock import Clock

#: Job states from the schema's enum.
TERMINAL_STATES = frozenset({"succeeded", "failed", "cancelled", "interrupted"})

#: Job kinds from the schema's enum, with whether `jobs/{id}/cancel` applies.
#: `network_apply` is false on the contract's instruction that a network job is
#: cancelled through the transaction's `DELETE`, never through the jobs route.
CANCELLABLE_KINDS: dict[str, bool] = {
    "matter_open": False,
    "matter_close": False,
    "network_apply": False,
    "network_discard": False,
    "wifi_scan": True,
    "upload_chunk": False,
    "firmware_verify": True,
    "firmware_delete": False,
    "coprocessor_update": True,
    "password_change": False,
    "system_update": True,
}


@dataclass(frozen=True)
class Step:
    """One phase of a job, with how long it takes and what it counts.

    `cancellable` narrows a cancellable kind to the steps before its destructive
    phase: a coprocessor update may be cancelled until it starts erasing, never
    after (the contract's example carries `cancellable: false` in `writing`)."""

    phase: str
    duration_ms: int
    unit: str | None = None
    total: int | None = None
    state: str = "running"
    on_done: Callable[[], None] | None = None
    cancellable: bool = True


@dataclass
class Job:
    """One job. Read it through `Job.settled()`; do not read the fields raw."""

    id: str
    kind: str
    resource_url: str | None
    boot_id: str
    created_ms: int
    park_state: str | None = None
    final_state: str = "succeeded"
    steps: list[Step] = field(default_factory=list)

    state: str = "queued"
    phase: str = "queued"
    progress: dict[str, object] | None = None
    error: ApiError | None = None
    updated_ms: int = 0
    parked: bool = False

    _cursor_ms: int = 0
    _index: int = 0

    def __post_init__(self) -> None:
        self._cursor_ms = self.created_ms
        self.updated_ms = self.created_ms
        if self.kind not in CANCELLABLE_KINDS:
            raise ValueError(f"not a contract job kind: {self.kind!r}")

    @property
    def cancellable(self) -> bool:
        """Cancellable only while there is something left to cancel. The
        contract's own example carries `cancellable: false` on a job whose
        destructive phase has started, and a terminal job is never cancellable
        however permissive its kind."""
        if not CANCELLABLE_KINDS[self.kind] or self.state in TERMINAL_STATES:
            return False
        if self._index >= len(self.steps):
            # Past its steps and not terminal: parked. It stays as cancellable as
            # its last step was - a system update parked in `rebooting` is not.
            return self.steps[-1].cancellable if self.steps else True
        return self.steps[self._index].cancellable

    @property
    def is_terminal(self) -> bool:
        return self.state in TERMINAL_STATES

    # -- progression -----------------------------------------------------

    def settle(self, now_ms: int) -> None:
        """Advance to what the clock says, firing each step's effect once."""
        if self.is_terminal:
            return
        while self._index < len(self.steps):
            step = self.steps[self._index]
            elapsed = now_ms - self._cursor_ms
            if elapsed < step.duration_ms:
                self._enter(step, elapsed, now_ms)
                return
            self._cursor_ms += step.duration_ms
            self._index += 1
            if step.on_done is not None:
                step.on_done()
            self._touch(now_ms)
        if self.park_state is not None:
            if not self.parked:
                self.parked = True
                self.state = self.park_state
                self.phase = self.steps[-1].phase if self.steps else self.phase
                self.progress = self._full(self.steps[-1]) if self.steps else None
                self._touch(now_ms)
            return
        if self.state != self.final_state:
            self.state = self.final_state
            self.progress = self._full(self.steps[-1]) if self.steps else None
            self._touch(now_ms)

    def _enter(self, step: Step, elapsed: int, now_ms: int) -> None:
        changed = self.state != step.state or self.phase != step.phase
        self.state = step.state
        self.phase = step.phase
        if step.unit is None:
            self.progress = None
        else:
            total = step.total
            fraction = elapsed / step.duration_ms if step.duration_ms else 1.0
            completed = int((total or 0) * fraction) if total is not None else int(fraction * 100)
            self.progress = {"completed": completed, "total": total, "unit": step.unit}
        if changed:
            self._touch(now_ms)

    def _full(self, step: Step) -> dict[str, object] | None:
        if step.unit is None:
            return None
        return {"completed": step.total or 0, "total": step.total, "unit": step.unit}

    def _touch(self, now_ms: int) -> None:
        self.updated_ms = now_ms

    # -- external resolution ---------------------------------------------

    def resume(self, now_ms: int, steps: Iterable[Step], final_state: str = "succeeded") -> None:
        """Take a parked job onward. Time spent parked does not count against
        the new steps: the world was waiting for a person, not for work."""
        if not self.parked:
            raise ValueError(f"job {self.id} is not parked (state {self.state})")
        self.parked = False
        self.park_state = None
        self.final_state = final_state
        self._cursor_ms = now_ms
        self.steps.extend(steps)
        self.settle(now_ms)

    def finish(self, now_ms: int, state: str, error: ApiError | None = None) -> None:
        """End the job here, whatever it was doing. Used for a cancellation and
        for a timeout that the job itself cannot observe."""
        if state not in TERMINAL_STATES:
            raise ValueError(f"not a terminal state: {state!r}")
        self.parked = False
        self.park_state = None
        self.state = state
        self.error = error
        self._index = len(self.steps)
        self._touch(now_ms)

    # -- serialisation ---------------------------------------------------

    def to_json(self, uptime_base_ms: int = 0) -> dict[str, object]:
        return {
            "id": self.id,
            "boot_id": self.boot_id,
            "kind": self.kind,
            "state": self.state,
            "phase": self.phase,
            "progress": self.progress,
            "cancellable": self.cancellable,
            "created_uptime_ms": str(max(0, self.created_ms - uptime_base_ms)),
            "updated_uptime_ms": str(max(0, self.updated_ms - uptime_base_ms)),
            "resource_url": self.resource_url,
            "error": _error_to_json(self.error),
        }


def _error_to_json(err: ApiError | None) -> dict[str, object] | None:
    """An `ErrorDetail` inside a larger document, per the contract's rule that a
    failure after a `202` is recorded in the job with the same shape a rejection
    would have had."""
    return None if err is None else detail_dict(err)


class JobStore:
    """Every job the mock has created, settled on demand.

    Retention is not simulated. The contract bounds it at 16 full terminal
    records plus 256 compact summaries, and `job-manager`'s sim suite covers
    eviction and the `429` at capacity; reproducing it here would give the
    frontend a job that vanishes mid-poll for reasons the device would not
    share, which is a worse mock rather than a more faithful one.
    """

    def __init__(self, clock: Clock, boot_id: str) -> None:
        self._clock = clock
        self._boot_id = boot_id
        self._jobs: dict[str, Job] = {}
        self._counter = 0

    @property
    def boot_id(self) -> str:
        return self._boot_id

    def create(
        self,
        kind: str,
        steps: Iterable[Step],
        resource_url: str | None = None,
        park_state: str | None = None,
        final_state: str = "succeeded",
    ) -> Job:
        self._counter += 1
        now = self._clock.now_ms()
        job = Job(
            id=f"job_{self._counter:04x}",
            kind=kind,
            resource_url=resource_url,
            boot_id=self._boot_id,
            created_ms=now,
            park_state=park_state,
            final_state=final_state,
            steps=list(steps),
        )
        self._jobs[job.id] = job
        job.settle(now)
        return job

    def get(self, job_id: str) -> Job | None:
        job = self._jobs.get(job_id)
        if job is not None:
            job.settle(self._clock.now_ms())
        return job

    def settle_all(self) -> None:
        now = self._clock.now_ms()
        for job in self._jobs.values():
            job.settle(now)

    def active_ids(self) -> list[str]:
        self.settle_all()
        return [job.id for job in self._jobs.values() if not job.is_terminal][:16]

    def __len__(self) -> int:
        return len(self._jobs)
