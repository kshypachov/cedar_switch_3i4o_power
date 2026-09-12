# SPDX-License-Identifier: Apache-2.0
"""One clock, readable in real time and skippable on demand.

The state machines worth mocking are the ones with timers in them: a 300-second
candidate lifetime, a 120-second confirmation window, a commissioning window of
up to 900 seconds. Two requirements pull in opposite directions — a frontend
developer wants those timers to run at real speed so the countdown in the UI
looks right, and a test wants to reach the timeout without waiting two minutes.

Both are the same clock with an offset. `now_ms()` is elapsed monotonic time
plus whatever has been skipped, so the server keeps real time until something
asks it to jump, and a test that freezes the source gets a clock that moves
only when told. There is no separate fake, so the thing under test in a test is
the thing that runs in the server.
"""

from __future__ import annotations

import time
from collections.abc import Callable


class Clock:
    """Monotonic milliseconds since this clock was created, plus skipped time."""

    def __init__(self, source: Callable[[], int] | None = None) -> None:
        self._source = source or (lambda: time.monotonic_ns() // 1_000_000)
        self._origin = self._source()
        self._offset_ms = 0

    def now_ms(self) -> int:
        return self._source() - self._origin + self._offset_ms

    def advance(self, ms: int) -> int:
        """Skip forward. Negative is refused: a timer that can go backwards is
        a state machine with no invariants left."""
        if ms < 0:
            raise ValueError("a monotonic clock does not go backwards")
        self._offset_ms += ms
        return self.now_ms()


class FrozenSource:
    """A source that only moves when stepped, for tests that want no real time
    in the measurement at all."""

    def __init__(self, start: int = 0) -> None:
        self.value = start

    def __call__(self) -> int:
        return self.value


def frozen_clock(start: int = 0) -> Clock:
    return Clock(source=FrozenSource(start))
