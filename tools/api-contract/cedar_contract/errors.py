# SPDX-License-Identifier: Apache-2.0
"""The device's rejection, reproduced byte for byte.

The point of this module is that a frontend cannot tell the mock from the
device by the way either one says no. That is a stronger requirement than
"returns the same fields", so everything here is a transcription of
`modules/api-validation/lib/api_validation.c` rather than an independent
reading of the contract:

- `ERROR_TABLE` is the C `error_table`, in the same order, carrying the same
  three facets of one decision: the wire name, the HTTP status, and
  `retryable`. As in C there is no way to override the status or the flag at a
  call site, because both are properties of the code.
- `serialise()` is the C `api_error_to_json()`, not `json.dumps`. The two
  differ in ways a frontend would see: C emits `\\u0008` where Python emits
  `\\b`, and C passes UTF-8 straight through where Python escapes it. Writing
  the serialiser out is the only way the claim survives contact with a
  non-ASCII message.
- The bounds are the C bounds. A message is truncated at 127 *bytes* and a
  path at 63, which is what `copy_bounded()` does, multi-byte characters
  included — the device would split one and so does this.
- Overflowing the field list is recorded the same way, by appending
  `TRUNCATION_NOTE` to the message, because `ErrorDetail` is
  `additionalProperties: false` and the message is the only place it fits.

`tests/test_error_body.py` parses the C source and fails if any of that drifts,
which is the only reason to trust the word "transcription" here.
"""

from __future__ import annotations

import random
from collections.abc import Iterable
from dataclasses import dataclass, field

#: Wire name -> (HTTP status, retryable). Order matches the C enum.
ERROR_TABLE: dict[str, tuple[int, bool]] = {
    "invalid_json": (400, False),
    "invalid_query": (400, False),
    "invalid_cursor": (400, False),
    "authentication_required": (401, False),
    "invalid_credentials": (401, False),
    # Retryable: logging in again is exactly what the client should do.
    "session_expired": (401, True),
    "csrf_failed": (403, False),
    "origin_rejected": (403, False),
    "setup_not_allowed": (403, False),
    "not_found": (404, False),
    # A conflict of timing may clear; a conflict of content never does.
    "busy": (409, True),
    "stale_revision": (409, False),
    "invalid_state": (409, False),
    "offset_mismatch": (409, False),
    "idempotency_conflict": (409, False),
    "ethernet_required": (409, False),
    "resource_expired": (410, False),
    "boot_changed": (410, False),
    "payload_too_large": (413, False),
    "unsupported_media_type": (415, False),
    "validation_failed": (422, False),
    "invalid_image": (422, False),
    "unsupported_target": (422, False),
    "incompatible_firmware": (422, False),
    "signature_invalid": (422, False),
    "rate_limited": (429, True),
    "internal_error": (500, True),
    "service_not_ready": (503, True),
    # Not retryable, unlike its neighbour: a capability the build does not
    # have, and waiting does not add one.
    "capability_unavailable": (503, False),
    "storage_full": (507, False),
}

#: The closed set `api-validation` defines for `ErrorField.code`, in C order.
FIELD_CODES: tuple[str, ...] = (
    "required",
    "invalid_format",
    "out_of_range",
    "too_long",
    "unknown_field",
    "conflicting",
    "not_allowed",
)

#: Appended to the message when the field list overflowed. From the C source.
TRUNCATION_NOTE = " (further problems were not reported)"

#: API_ERROR_MESSAGE_MAX_LEN, API_ERROR_PATH_MAX_LEN, the Kconfig default.
MESSAGE_MAX_BYTES = 127
PATH_MAX_BYTES = 63
MAX_FIELDS = 6


def status_of(code: str) -> int:
    """HTTP status for a code. Unknown codes are a bug, and 500 is the only
    defensible answer: it promises the client nothing about what went wrong."""
    return ERROR_TABLE[code][0] if code in ERROR_TABLE else 500


def is_retryable(code: str) -> bool:
    return ERROR_TABLE[code][1] if code in ERROR_TABLE else False


class RequestIds:
    """`req_%08x` off a counter, seeded once per process.

    The device seeds from the cycle counter and explains why that is enough: a
    request id is a correlation handle that is never accepted as input, so it
    needs no entropy. Same reasoning, same format, so a frontend that pattern
    matches on it works against both.
    """

    def __init__(self, seed: int | None = None) -> None:
        self._state = random.getrandbits(32) if seed is None else seed & 0xFFFFFFFF

    def next(self) -> str:
        self._state = (self._state + 1) & 0xFFFFFFFF
        return f"req_{self._state:08x}"


#: One counter for the process, as the device has one counter for the boot.
#: Every `ApiError` takes its id from here, so an error a timer produced — a
#: confirmation that timed out, a candidate that expired — carries a real
#: correlation handle instead of a placeholder, and is drawn from the same
#: sequence as the ids on responses.
REQUEST_IDS = RequestIds()


def next_request_id() -> str:
    return REQUEST_IDS.next()


def _truncate_utf8(text: str, limit: int) -> bytes:
    """Cut to `limit` bytes the way the device's `copy_bounded()` does.

    It copies bytes until the buffer is full, so a multi-byte character at the
    boundary is split and the result is not valid UTF-8. That is reproduced
    rather than fixed: the frontend has to survive whatever the device sends,
    and a mock that quietly behaves better hides the case.
    """
    return text.encode("utf-8", "surrogatepass")[:limit]


_ESCAPES = {
    0x22: b'\\"',
    0x5C: b"\\\\",
    0x0A: b"\\n",
    0x0D: b"\\r",
    0x09: b"\\t",
}


def _escape(raw: bytes) -> bytes:
    """The device's `append_escaped()`: five named escapes, `\\u00xx` for the
    rest of the control range, and every other byte straight through."""
    out = bytearray()
    for byte in raw:
        esc = _ESCAPES.get(byte)
        if esc is not None:
            out += esc
        elif byte < 0x20:
            out += b"\\u%04x" % byte
        else:
            out.append(byte)
    return bytes(out)


@dataclass
class ErrorField:
    path: str
    code: str

    def __post_init__(self) -> None:
        if self.code not in FIELD_CODES:
            raise ValueError(f"not a field code in the closed set: {self.code!r}")
        if not self.path.startswith("/"):
            raise ValueError(f"field path is a JSON Pointer: {self.path!r}")


@dataclass
class ApiError(Exception):
    """A rejection as a value, the way `struct api_error` is a value.

    `fields` beyond `MAX_FIELDS` are dropped and the loss is recorded, exactly
    as `api_error_add_field()` does. `retry_after_seconds` is carried here but
    serialised as a header, never into the body — the contract puts it in
    `Retry-After`.
    """

    code: str
    message: str = ""
    fields: list[ErrorField] = field(default_factory=list)
    request_id: str = field(default_factory=next_request_id)
    retry_after_seconds: int | None = None
    fields_truncated: bool = False

    def __post_init__(self) -> None:
        if self.code not in ERROR_TABLE:
            raise ValueError(f"not a contract error code: {self.code!r}")
        if len(self.fields) > MAX_FIELDS:
            self.fields = self.fields[:MAX_FIELDS]
            self.fields_truncated = True
        super().__init__(self.code)

    @property
    def status(self) -> int:
        return status_of(self.code)

    @property
    def retryable(self) -> bool:
        return is_retryable(self.code)

    def add_field(self, path: str, code: str) -> None:
        if len(self.fields) >= MAX_FIELDS:
            self.fields_truncated = True
            return
        self.fields.append(ErrorField(path, code))

    def to_json(self) -> bytes:
        return serialise(self)


def serialise(err: ApiError) -> bytes:
    """`api_error_to_json()`: no whitespace, this key order, `fields` omitted
    rather than sent empty.

    An empty array reads as "we checked the fields and they are fine", which is
    not what a rejection with no field detail means.
    """
    message = err.message
    body = bytearray(b'{"error":{"code":"')
    body += err.code.encode("ascii")
    body += b'","message":"'
    body += _escape(_truncate_utf8(message, MESSAGE_MAX_BYTES))
    if err.fields_truncated:
        body += _escape(TRUNCATION_NOTE.encode("ascii"))
    body += b'","request_id":"'
    body += _escape(err.request_id.encode("utf-8"))
    body += b'","retryable":'
    body += b"true" if err.retryable else b"false"
    if err.fields:
        body += b',"fields":['
        for index, item in enumerate(err.fields):
            if index:
                body += b","
            body += b'{"path":"'
            body += _escape(_truncate_utf8(item.path, PATH_MAX_BYTES))
            body += b'","code":"'
            body += item.code.encode("ascii")
            body += b'"}'
        body += b"]"
    body += b"}}"
    return bytes(body)


def bounded_message(err: ApiError) -> str:
    """The message as it will appear on the wire: cut to the device's buffer,
    with the truncation note appended if the field list overflowed."""
    text = _truncate_utf8(err.message, MESSAGE_MAX_BYTES).decode("utf-8", "replace")
    return text + (TRUNCATION_NOTE if err.fields_truncated else "")


def detail_dict(err: ApiError) -> dict[str, object]:
    """The same `ErrorDetail`, as a structure rather than as bytes.

    A `Job` and a `NetworkTransaction` carry an `ErrorDetail` inside a larger
    document, so they cannot use the pre-serialised bytes. Key order matches
    `serialise()` so the two agree on the wire, and the message goes through the
    same bounding. The one difference is unreachable in practice: where the
    device's byte copy would split a multi-byte character, this yields U+FFFD
    instead, and every message the mock produces is ASCII.
    """
    detail: dict[str, object] = {
        "code": err.code,
        "message": bounded_message(err),
        "request_id": err.request_id,
        "retryable": err.retryable,
    }
    if err.fields:
        detail["fields"] = [{"path": f.path, "code": f.code} for f in err.fields]
    return detail


def error(code: str, message: str = "", fields: Iterable[tuple[str, str]] = ()) -> ApiError:
    """Shorthand for raising: `raise error("busy", "...", [("/x", "required")])`."""
    err = ApiError(code=code, message=message)
    for path, field_code in fields:
        err.add_field(path, field_code)
    return err
