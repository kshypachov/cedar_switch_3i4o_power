# api-contract

The mock server the frontend is built against, and the checks that hold
`openapi.json` to its own rules. Two of stage P1's deliverables, in one package
because they share the same two facts: what the document says, and what the
device's rejection looks like.

Contract: [`api-contract.md`](../../docs/device-development/api-contract.md) for
behaviour, [`openapi.json`](../../docs/device-development/openapi.json) for
shapes, section 12 of the
[development plan](../../docs/device-development/development-plan.md) for what is
required of both. The module docstrings carry the design; this file covers how it
fits the system and what was decided along the way.

## Running

```sh
tests/ci/run-contract-tests.sh            # the checks and the suite, one command
tests/ci/run-contract-tests.sh -k network # extra arguments go to pytest
```

No container, unlike the sim tier — this is plain Python with no Zephyr in it.

To serve the mock for frontend work:

```sh
cd tools/api-contract
python -m cedar_contract.mock --port 8080
python -m cedar_contract.mock --scenario setup_required=false --scenario fabrics=2
```

It prints the credentials it will accept. The device it imitates starts
**unconfigured**: `GET /api/v1/auth/state` answers `setup_required: true` and
publishes the setup token, as the device does since P2 (owner's decision: the
token is shown in the web interface); `POST /api/v1/auth/setup` needs it in
`X-Setup-Token` (`cedar-mock-setup-token`). With `setup_required=false` the
administrator password is `cedar-mock-admin`. Neither is a secret; both are
printed at startup.

With `--ui <dist>` it also serves a built frontend at `/`, on the same origin as
the API, with the device's static policy (`mock/static.py`): the same files,
MIME types, gzip, ETags, cache classes and Content-Security-Policy, because it
runs the firmware's own generator and reads the policy out of
`modules/web-assets/lib/web_assets.c`. That is what the frontend's end-to-end
suite runs against:

```sh
python -m cedar_contract.mock --port 8099 --ui ../../src/web/frontend/dist
```

Requests reach the state machines one at a time, as on the device; connections
each get a thread, because a browser keeps one open and asks for the next file on
another.

The document checks can also be run alone, which is what a pre-commit hook would
want:

```sh
cd tools/api-contract && python -m cedar_contract.checks
```

## Why Python

It is already a hard dependency of the toolchain — west and twister are Python,
and `tests/ci/Dockerfile` installs an interpreter and `jsonschema` — so the mock
adds no runtime to install or to keep current. Node arrives in P2 for the
frontend and for generating types from the document, but the mock does not need
it: it speaks HTTP, and nothing on the other end can tell what it is written in.
`jsonschema` does the schema validation rather than something hand-rolled, for
the same reason and one more: a 2020-12 validator written here would be less
trustworthy than the one it replaced.

## What the mock is, at two levels

**Coverage.** All thirty-five declared operations answer with something that
validates against their own response schema, at the status the document declares.
This is the contract test section 12 asks for, and it is why the generated client
of P2 can be exercised at all.

**State.** The flows with a state machine behave the way the contract says,
because a frontend cannot be debugged against a server that always says yes:

| Flow | What the mock actually does |
|---|---|
| auth | `setup_required` on a fresh device, setup closes itself, session cookie with the HTTP-profile flags, CSRF token, idle and absolute lifetimes, password change as a job that revokes every session |
| jobs | `queued → running → succeeded` over time, phases in order, progress restarting at zero with each phase, `waiting_confirmation` as a park rather than an end |
| network | `revision`, `pending_transaction_id`, `staged → applying → awaiting_confirmation → committed`, rollback on `DELETE`, rollback on confirmation timeout, `409 stale_revision`, candidate expiry at 300 s |
| wifi scan | `202` and a job, then results under the job id; one scenario fills the 64-record limit and reports `truncated=true` |
| upload | `received_bytes` advancing only when the chunk's job completes, `409 offset_mismatch`, `verify` reaching `ready`, `capability_unavailable` for OTA |
| capabilities | `esp32_ota.available=false` with `reason="not_implemented"`, so the UI shows the reason rather than a disabled placeholder |

Everything else answers with a fixed, plausible snapshot. Logs are the one
in-between case: the ring's hard parts belong to `log-store`'s sim suite, but the
cursor arithmetic is real, because a cursor that never advanced would make the
log screen page forever.

## The error body

`errors.py` is a transcription of
[`api_validation.c`](../../modules/api-validation/lib/api_validation.c), not an
independent reading of the contract, and `tests/test_error_body.py` parses the C
source to keep it honest: the error table row for row, the closed set of field
codes, the truncation note, and the three bounds. The serialiser is written out
rather than delegated to `json.dumps`, because the two differ where a frontend
would see it — C spells a backspace `\u0008` where Python spells it `\b`, and
C passes UTF-8 straight through where Python escapes it.

The mock also turns a schema violation into the contract's field codes, so a
malformed request is refused with a JSON Pointer and a code per bad field, and
the frontend's error handling meets the shape it will meet on the device.

## Three decisions the contract does not make

The contract names a code for nearly every situation. Where it does not, the mock
chose, and the choice is recorded here so the device can be made to match rather
than quietly differ. Each is also marked `DECISION` in the module that makes it.

| Situation | Chosen | Why |
|---|---|---|
| Wrong `current_password` on `PUT /auth/password` | `401 invalid_credentials`, synchronously | The contract returns `202` here and says failures after a `202` are recorded in the job — but the rule it states covers hardware and SDK failures, not a credential the server can check before accepting any work. The frontend gets the answer on the request that carried the password. |
| Confirmation timeout on a network transaction | job fails with `resource_expired` | The transaction is exactly the non-transferable RAM resource the 410 row describes, and its recovery — re-read `network/config`, stage again — is the one that code implies. `busy` and `invalid_state` would both suggest retrying the same confirm, which can never work. |
| A required header missing or malformed | `422 validation_failed`, no `fields` | `fields` is a list of JSON Pointers into the request body; inventing a pointer syntax for headers would put a shape in the error body that the document does not describe. The header is named in the message instead. |

Two further behaviours are the mock's own, not the device's, and are called out
where they happen: closing a commissioning window leaves a fabric behind (on the
device that needs a real controller, and without it no frontend test reaches the
populated fabric table), and `install_transport` stands in for the Ethernet check
the mock cannot actually observe.

## Rules the device and the mock now share

P2 wrote the device's side of the request pipeline (`modules/web-api`) and, where
Python's libraries answer an edge of JSON or HTTP differently from the C code,
changed the mock so a frontend cannot tell the two apart. Each rule is in the
device's headers, and each is tested on both sides.

| Rule | Why |
|---|---|
| Every `/api/` request needs a Host that is an IP literal, `localhost` or a configured name (`extra_hosts`), else `403 origin_rejected` — checked before routing | `/auth/state` publishes the setup token, and a DNS-rebinding page is same-origin with the device; it cannot send an IP literal as Host |
| Duplicate member names, `NaN`/`Infinity`, bytes that are not UTF-8, nesting deeper than 8, more than 64 members in an object: `400 invalid_json` | the device decodes by hand with those bounds; I-JSON for duplicates |
| An integer is a value: `120.0` and `1.2e2` are 120; a magnitude past any bound is `out_of_range` without being built | JSON Schema's meaning of "integer", and the mock must not construct a 100-million-digit number |
| Every undeclared member is reported, not only the first | the old message-parsing took one; the device reports all |
| A nullable field reports its real problem (`too_long`), not `invalid_format` from `anyOf` | to the device it is a nullable string |
| A string holding U+0000 or an unpaired surrogate is `invalid_format` | a C string cannot hold it; Python can |
| One entry per value, by precedence too_long, out_of_range, invalid_format, not_allowed; entries sorted by pointer segment, indices numerically; a segment with a control character, or one that would make the pointer longer than 63 bytes, is left off | the same list on both sides, so the same entries survive truncation |

## The control plane

`/__mock/*` — outside `/api/v1`, which the device never serves, so nothing here
can be mistaken for the contract:

| Request | Effect |
|---|---|
| `GET /__mock/state` | a debugging dump: uptime, revision, scenario, staged transaction, upload |
| `POST /__mock/reset` | a fresh device, optionally `{"scenario": {...}}` |
| `POST /__mock/advance` | `{"seconds": N}` skips time |
| `POST /__mock/scenario` | change one knob without a reset |

It exists for two things a frontend has to handle that are otherwise unreachable
in a test: a 120-second confirmation timeout, and the paths only a
differently-behaving device produces. A frontend that came to depend on it fails
against the real device immediately, which is the right way for that mistake to
surface.

Scenario knobs are in `mock/scenario.py`. They are named rather than random on
purpose: a mock whose failures cannot be reproduced is worse than one that always
succeeds.

## The document checks

Section 12 lists five, and since P2 all five run. The fifth waited in
`checks.DEFERRED` until the device had a route table to compare.

| Check | What it refuses |
|---|---|
| `refs_resolve` | a `$ref` that lands nowhere, and any reference out of the file — the frontend generates its types from this one document, and an external reference would make that build need the network |
| `path_parameters` | a placeholder with no declaration, a declaration with no placeholder, a path parameter that is not `required`, one with no schema |
| `operation_ids` | a missing or duplicated `operationId`; the generated client names its methods from these, so a duplicate silently drops an operation from the frontend's surface |
| `examples_valid` | an example that violates the schema it illustrates, formats included — the fixtures are built from these, so a bad example produces a test suite that passes against data the device will never send |
| `undocumented_routes` | a route in `src/web/api/v1/routes.h` that the document does not declare, or declares with another method or path; flags that disagree with the document (public ⇔ `security: []`, CSRF, Idempotency-Key and setup-token routes ⇔ those header parameters, a decoded body ⇔ `requestBody` and its `required`, query names ⇔ query parameters); a route path with no server resource in `http_resources.h`, or a resource with no route. Operations the device does not serve yet are printed, not failed: each belongs to a later stage |

Two of the four already passed when they were written, by hand. That is not an
argument against automating them: the value of a check is the day it fails, and a
check nobody runs has no such day.

`format` needed a decision of its own. `jsonschema`'s default checker covers
`ipv4` and `ipv6` from the standard library but leaves `date-time` and `uri`
unchecked unless two more packages are installed. Both are registered in
`openapi.py` against the standard library instead: `wall_time` and
`reconnect_urls` are exactly the fields a careless mock would fill with something
a browser cannot parse, and an unchecked format in a contract test is a silent
hole.

## Testing

233 tests. The schema check is not one of them — it is in `Client._check` in
`tests/conftest.py` and runs on every call in every file, for successes and for
rejections alike, along with the assertion that a rejection's `request_id`
matches its `X-Request-ID` header. If only one test validated responses, every
other test would be free to assert on a body the device could never send.

Layout:

| File | Covers |
|---|---|
| `test_error_body.py` | the transcription against the C source, and the serialiser |
| `test_document_checks.py` | each check on the real document, and on a copy with one thing broken |
| `test_mock_coverage.py` | all thirty-five operations reached, at their declared statuses |
| `test_mock_auth.py` | setup, login, session lifetimes, the password change |
| `test_mock_jobs.py` | the sequence a poller sees, cancellation, parking |
| `test_mock_network.py` | the transaction's every edge, and the business rules with their field codes |
| `test_mock_wifi.py` | the awkward access points, and `truncated` |
| `test_mock_matter.py` | the window, the codes, the fabric table |
| `test_mock_firmware.py` | the offset rule, verification outcomes, the install |
| `test_mock_logs.py` | filters, the cursor, the export |
| `test_mock_middleware.py` | the order of the checks, idempotency, bodies, routing |
| `test_mock_control.py` | `/__mock` |
| `test_mock_server.py` | the HTTP adapter over a real socket |
| `test_mock_parsing.py` | the rules shared with the device's decoder and Host check |
| `test_mock_static.py` | the frontend as `--ui` serves it, against the device's web-assets policy |

The suite was checked against seventy-one mutations of the implementation — one
decision broken at a time, from a flipped `retryable` flag to a confirmation
timeout that never fires to a check that reports nothing. Four survived the first
pass and each named a real gap: a `network_apply` job advertising `cancellable`
that the route would refuse, an `Idempotency-Key` absent rather than merely
malformed, the ordering of authentication before CSRF, and a rejection's
`request_id` not matching its header. All four are now covered, and a second pass
caught all seventy-one.

## What this is not

It holds no device logic. `received_bytes` advances after a timer, not after a
write; a candidate is validated but nothing is configured; a job's phases are
durations, not work. The state machines are faithful and everything underneath
them is a fixture — which is the point, and also the reason a green run here says
nothing about LittleFS, the W5500 or the C6. Section 12 keeps those on the
hardware tier, and the plan's stage report counts them separately.
