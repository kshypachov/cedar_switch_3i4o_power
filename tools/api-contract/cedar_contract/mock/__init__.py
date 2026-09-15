# SPDX-License-Identifier: Apache-2.0
"""A mock of the device's web API, good enough to build a frontend against.

Stage P1's criterion is "the mock is usable by the frontend". Two levels follow
from that, and they are deliberately not the same thing:

**Coverage.** All thirty-seven declared operations answer with something that
validates against their own response schema. This is what the contract test
asserts, and it is what lets the generated client be exercised at all.

**State.** The flows that have a state machine behave the way
`api-contract.md` says they do, because a frontend cannot be debugged against
a server that always says yes:

- `GET /jobs/{id}` moves through its phases over time. A mock that answered
  `succeeded` immediately would make the polling loop in the UI unverifiable.
- The network transaction runs `staged -> applying -> awaiting_confirmation ->
  committed`, rolls back on `DELETE`, rolls back on confirmation timeout, and
  answers `409 stale_revision` to a candidate built on a revision that moved.
- Wi-Fi scan is `202` plus a job, then results; one scenario reports
  `truncated=true`.
- Upload advances `received_bytes` only when a chunk's job completes, rejects a
  wrong offset with `409 offset_mismatch`, and reaches `ready` through `verify`.
- Capabilities report `esp32_ota.available=false` with
  `reason="not_implemented"`, so the UI shows the reason rather than a disabled
  placeholder switch.

Everything else answers with a plausible fixed snapshot.

Structure mirrors the testability condition section 12 puts on the C modules: a
core that decides and a thin adapter that does I/O. `app.MockApp.handle()` maps
a request value to a response value with no sockets involved, and `server.py`
is the only part that knows about HTTP transport. That is why the mock's own
tests are the contract test — they drive the same code path a browser does.
"""
