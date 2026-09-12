# job-manager

Identity, lifecycle and idempotency for every long-running operation the web
API exposes. A request that cannot finish inside an HTTP handler creates a job,
answers `202` with its id, and the client polls it.

Contract: `docs/device-development/api-contract.md`, section "Задачи", and the
`Job` schema in `openapi.json`. The public header carries the full API
documentation; this file covers only how the module fits the system.

## Why it exists

Two rules from the contract drive the whole design.

**`202` is not success.** A job owns its terminal outcome, including the error
detail, so a failure that hardware reports minutes later is never retro-fitted
into an HTTP status that has already been sent.

**A retry must not act twice.** The same idempotency key with the same request
returns the same job. The same key with a different request is a conflict, not
a second execution.

## Lifecycle and threads

No threads of its own. Callers drive state from wherever the work runs; the
module only enforces which transitions are legal and timestamps them. Every
entry point takes an internal mutex and snapshots are copied out, so an HTTP
handler can format JSON from one without holding a lock. Not ISR-safe.

## Retention

Two tiers, both fixed-size, no dynamic allocation:

- **Full records** (`CONFIG_JOB_MANAGER_MAX_JOBS`, default 16) hold the complete
  job including phase and progress.
- **Compact records** (`CONFIG_JOB_MANAGER_DEDUPE_SLOTS`, default 256) outlive
  them, keeping id, key and outcome so a late retry still gets a truthful
  answer instead of triggering the action again.

Active jobs are never evicted from either tier. When every full record holds
work still in flight, `job_create()` returns `JOB_CREATE_EXHAUSTED` and the
caller answers `429` — losing the identity of running work is the one outcome
the contract forbids outright.

Retention runs on the monotonic clock and does not survive a reboot, which is
correct: job ids are unique only within a boot, and the API pairs every id with
a boot id for exactly that reason.

## Testing

`tests/job_manager` covers the state machine, idempotency, retention, eviction
and the wire names, entirely in the sim tier. The clock is injectable, so
expiry is tested without sleeping.

Run it:

```sh
tests/ci/run-sim-tests.sh
```

Zephyr's POSIX architecture does not build on macOS, so the sim tier runs in a
Linux container; see `tests/ci/README.md`.
