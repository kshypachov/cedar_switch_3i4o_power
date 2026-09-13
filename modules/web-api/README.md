# web-api

The API's request pipeline: routing, the checks the contract orders, strict JSON
in and out, the helpers handlers answer with, and the adapter to Zephyr's HTTP
server.

Contract: "Общие правила" in
[`api-contract.md`](../../docs/device-development/api-contract.md) and
[`openapi.json`](../../docs/device-development/openapi.json). The headers carry
the design (`web_api.h`, `json_reader.h`, `json_writer.h`, `web_api_http.h`);
this file covers how it fits the system and what was measured before it was
designed.

## Shape

- **Core**: `web_api_dispatch()` turns a `struct web_api_request` into a
  `struct web_api_response`. No socket, no timing. The middleware runs in the
  mock's order — Host, captured headers, route, session, Origin, CSRF, required
  headers, body, query, handler — so a bad request gets the same answer from
  both.
- **Route table**: `src/web/api/v1/routes.h`, one X-macro line per served
  operation. `tools/api-contract` reads the same file and checks it against the
  document (`python -m cedar_contract.checks`, check `undocumented_routes`).
- **Strict JSON**: `json_reader.c` decodes a body against a static schema
  description into a struct; `json_writer.c` writes responses with
  api-validation's escaper.
- **Adapter**: `web_api_http.c` captures headers, keeps one context per HTTP
  client, accumulates the body up to its limit, and releases the context on the
  server's COMPLETE or ABORTED.

## What Zephyr's server does, measured

`docs/device-development/reports/p2/http-server-behaviour` is a native_sim suite
run against this tree's server before anything here was written. Its facts
shaped the design:

| Observation | Consequence here |
|---|---|
| A dynamic response is always chunked, and 204/304 still get the zero-length terminating chunk | 204 and 304 are sent with `Connection: close` |
| A second client asking for a resource while the first is mid-body gets a bare `409 Conflict` (no body) and a closed connection | one server resource per API path (`http_resources.h`); the frontend retries a bare 409 |
| A client that closes mid-body produces ABORTED within 300 ms; one that stalls holds the resource until the inactivity timeout (10 s on the board) | contexts are released by those notifications; the stall is documented, not fixable in the adapter |
| A captured header longer than `MAX_HEADER_LEN` (32 by default) is dropped with only a status flag | `MAX_HEADER_LEN=256`; a dropped header is a 422, never a missing session |
| Wildcard resources are tried in name order, first match wins | resources are numbered in match order |
| Status lines have no reason phrase unless `COMPLETE_STATUS_PHRASES` | enabled |

## Decisions the contract did not make

- **Host is checked** on every `/api/` request: an IP literal, `localhost`, or a
  name in `CONFIG_WEB_API_EXTRA_HOSTS`. The setup token is published by
  `GET /auth/state`, and a DNS-rebinding page is same-origin with the device in
  the browser's eyes; it cannot put an IP literal in Host.
- **A header the server could not keep, or one sent twice, is 422
  `validation_failed`** without fields, like the mock's missing-header decision.
- **Idempotency scope**: the key given to job-manager is a 64-bit FNV-1a of
  principal, method, path and query in hex, a colon, and the client's key; the
  body hash is over the decoded struct, so spelling and member order are not
  content. `JOB_IDEMPOTENCY_KEY_MAX_LEN` became 96 for it.
- **JSON rules shared with the mock** (both implement them, both test them):
  duplicate member names are `invalid_json` (I-JSON); every undeclared member is
  reported; one entry per value with precedence too_long, out_of_range,
  invalid_format, not_allowed; entries sorted by pointer segment, indices
  numerically; `120.0` is the integer 120; a string that decodes to U+0000 or an
  unpaired surrogate is `invalid_format`.

## Memory and threads

Everything runs on the HTTP server's single cooperative thread. Contexts are
static: four clients × (8 KiB body + 4 KiB response + 2 KiB decoded body +
header storage) — `libweb_api.a` has 137 KB of `.bss`, linked into PSRAM by
`src/helpers/psram_sections.ld` (checked in the map). The JSON reader keeps its
state in static storage, so decoding is single-threaded, which it is.

A handler must not compute for long on this thread: nothing else in the system
runs meanwhile. The password derivation is the case that proved it (web-auth
README).

## Latency on the board

P0 method (15 sequential curl requests, each a new connection): `/api/v1/auth/state`
p50 89 / p95 97 ms, `/api/v1/system/status` p50 98 / p95 103 ms, the page `/`
p50 125 / p95 137 ms (P0's legacy static `/` was 53 / 56). One keep-alive
connection: status p50 73 / p95 76 ms. Two clients: p95 157 ms; four: p95 313 ms —
the server answers one request at a time. Time grows with the number of
response headers, which is consistent with Zephyr sending each header name,
value and CRLF as its own write; that is a hypothesis, not yet confirmed by a
packet capture. Details and the one method that misleads (a client sending
`Connection: close`, p95 1.09 s) are in the P2 report.

## Testing

- `tests/web_api` (sim, 77 cases): the reader's syntax and schema rules, the
  writer, Host and cookie parsing, every middleware step and its order on a test
  router, idempotency scope, and the real v1 bindings over web-auth with a fake
  platform — setup, login, 429, logout, the password change job with replay and
  conflict, system status, capabilities, jobs.
- `tests/web_api_http` (sim, 14 cases): the adapter and the firmware's resource
  list through Zephyr's real server over loopback, IPv4 and IPv6 — JSON 404 for
  every method, SPA fallback, gzip/406/304, fragmented body, 413 without
  buffering, abort, the stall timeout, two clients, dropped and repeated headers,
  foreign Host, setup/login/logout over the wire, every route reachable.

```sh
tests/ci/run-sim-tests.sh -s cedar.web_api -s cedar.web_api_http
```
