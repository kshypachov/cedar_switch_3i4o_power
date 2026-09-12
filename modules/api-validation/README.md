# api-validation

One place where a rejection gets its code, its HTTP status, its field paths and
its JSON shape, plus the strict parsers the contract needs for addresses and
opaque ids.

Contract: the "Общие правила" section of
`docs/device-development/api-contract.md` — the error table and the ErrorDetail
example — and the `Error`, `ErrorDetail` and `ErrorField` schemas in
`openapi.json`. The public header carries the full API documentation; this file
covers only how the module fits the system.

## Why it exists

Thirty-five operations reject requests. Without a shared home for that, each
handler invents its own: one returns `{"error": "..."}`, another
`{"message": "..."}`, a third answers `stale_revision` with 422 where its
neighbour answers 409. Every one of those is a contract violation that compiles
cleanly and is only noticed by whoever writes the frontend.

So the code is the unit of decision here, and three things follow from it rather
than being chosen alongside it:

- **The HTTP status.** The contract's table pairs them. Here the status is a
  property of the code, and there is no parameter to override it.
- **`retryable`.** It is a promise to the client about whether repeating the
  request could succeed, so it cannot vary by call site. The division is that a
  conflict of timing may clear and a conflict of content never does: `busy` and
  `service_not_ready` are retryable, `stale_revision` and `validation_failed`
  are not. `capability_unavailable` is not, because the contract uses it for a
  feature the build does not have — ESP32 OTA in this version — and waiting does
  not add one.
- **The JSON shape.** One serialiser, so a quote in a message or a path cannot
  break the document, and `fields` is omitted rather than sent empty. An empty
  array reads as "we checked the fields and they are fine", which is not what a
  rejection with no field detail means.

## Field errors

`ErrorField.code` is a free string in the contract, which shows only `required`.
This module defines a small closed set — `required`, `invalid_format`,
`out_of_range`, `too_long`, `unknown_field`, `conflicting`, `not_allowed` — so a
client can branch on it. Extending the set is a contract change like any other.

The list is bounded, and overflow is not silent: the rejection is marked
truncated and the serialiser appends a note to the message. `ErrorDetail` is
`additionalProperties: false`, so the message is the only place that note can
go. A client shown three of seven bad fields would otherwise fix three and be
rejected again with no hint that more were waiting.

## Parsers

Strict on purpose, and the interesting cases are all rejections. `010.0.0.1` is
refused rather than read as ten or as eight depending on which library gets it;
`1.2.3.4 ` with a trailing space is refused rather than trimmed; an IPv6 zone
identifier is refused rather than stored as a string the resolver cannot use. An
address that a lenient parser silently reinterprets is an address the operator
did not configure.

IPv4 and IPv6 are parsed here rather than through `net_addr_pton()` so the
module keeps no networking dependency and the sim tier does not have to bring up
a stack to test a string.

## What this module does not do

It holds no policy. Whether a static address needs a gateway, which interface
must stay reachable, whether a recovery path exists before an apply — those need
context this module does not have. `network-manager` composes the primitives
here (`api_ipv4_same_subnet`, `api_ipv4_is_usable_host`,
`api_ipv4_prefix_is_valid`) with its own rules, and section 12 of the plan puts
those invariants in its test table, not this one.

It also does not parse JSON. That belongs to `web-api`, which will use this
module to reject what it finds.

## Lifecycle and threads

No threads, no allocation, no I/O. An error is a value the caller owns, and
serialising it writes into a buffer the caller supplies. The only global state
is the request id counter, which is a plain increment seeded once per boot from
the cycle counter — a request id is a correlation handle that is never accepted
as input, so it needs no entropy and the module needs no random source.

A `struct api_error` is a few hundred bytes and belongs in a request context
rather than on a deep stack.

## Testing

`tests/api_validation` covers the whole error table against a transcription of
the contract's own, the serialiser including escaping and truncation, the
request id generator, and every parser and predicate — 23 tests in the sim tier.
`CONFIG_API_VALIDATION_MAX_FIELDS` is set to 2 in the test configuration so the
truncation path is reached rather than assumed. The suite was checked against
eleven mutations of the implementation, each of which it caught.

Run it:

```sh
tests/ci/run-sim-tests.sh -s cedar.api_validation
```
