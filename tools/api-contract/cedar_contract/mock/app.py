# SPDX-License-Identifier: Apache-2.0
"""Request in, response out, with the routing table read from the document.

Three decisions are worth stating before the code.

**The router is built from `openapi.json`, not written by hand.** A handler is
registered against an `operationId`, and registering one the document does not
declare raises at import time. The path template, the method, whether a session
is required, which headers are required, which content types the body may use
and which schema it must satisfy all come from the declaration. So the mock
cannot drift from the document in the direction that matters — it can only fail
to cover something, which the coverage test reports. It is also what makes the
plan's deferred fifth check cheap in P2: the device's route table gets diffed
against this same index.

**Middleware runs in the order a device would have to run it**, and that order
is a contract requirement rather than a preference: authentication before CSRF
(there is no token to check without a session), CSRF before the body (a rejected
request must not be parsed), size before parse (the limit exists so a large body
is never buffered), and the idempotency key before the handler, because a replay
must return the first answer without running the action twice.

**There is no HTTP here.** `handle()` takes a `Request` value and returns a
`Response` value. `server.py` is the only module that knows about sockets, so
the mock's own tests exercise exactly the code a browser reaches, without a
port, a timeout or a race.
"""

from __future__ import annotations

import decimal
import hashlib
import ipaddress
import json
from functools import cmp_to_key
from typing import Any
from urllib.parse import urlsplit

from ..errors import ApiError, error, next_request_id
from ..openapi import Document, Operation, path_to_regex
from . import handlers  # noqa: F401  (imported for the side effect of registering)
from .clock import Clock
from .constants import JSON_BODY_BYTES
from .scenario import Scenario
from .state import DeviceState
from .wire import (
    CSRF_HEADER,
    HANDLERS,
    IDEMPOTENCY_HEADER,
    SETUP_TOKEN_HEADER,
    Context,
    Request,
    Response,
    error_response,
    json_response,
)

#: Operations that mutate but are exempt from the idempotency key, per the
#: contract: the two that create a session and the one that ends it.
IDEMPOTENCY_EXEMPT = frozenset({"setupAdmin", "login", "logout"})

#: Operations that check `Origin` instead of a CSRF token, because they run
#: before a token exists.
ORIGIN_CHECKED = frozenset({"setupAdmin", "login"})


class MockApp:
    """The mock as a function from request to response."""

    def __init__(
        self,
        document: Document | None = None,
        clock: Clock | None = None,
        scenario: Scenario | None = None,
    ) -> None:
        self.doc = document or Document.load()
        self.clock = clock or Clock()
        #: The browser application, when served (`--ui`); see static.py.
        self.static: Any = None
        self.state = DeviceState(self.clock, scenario or Scenario())
        self._routes = [(path_to_regex(op.path), op) for op in self.doc.operations]
        unknown = set(HANDLERS) - set(self.doc.operation_ids())
        if unknown:
            raise RuntimeError(f"handlers for operations the document does not declare: {unknown}")

    @property
    def uncovered(self) -> tuple[str, ...]:
        """Declared operations with no handler. The coverage test asserts this
        is empty; keeping it as a property means the gap is reportable rather
        than discovered one 404 at a time."""
        return tuple(
            op.operation_id for op in self.doc.operations if op.operation_id not in HANDLERS
        )

    def match(self, method: str, path: str) -> Operation | None:
        """The operation a request would reach, or None.

        Exposed because the test client validates every response against the
        declared schema for the operation it hit, which turns every test in the
        suite into a contract test rather than leaving that to one of them.
        """
        base = self.doc.base_path
        if not path.startswith(base + "/"):
            return None
        tail = path[len(base) :].split("?")[0]
        for pattern, op in self._routes:
            if pattern.match(tail) and op.method == method:
                return op
        return None

    def reset(self, scenario: Scenario | None = None) -> None:
        self.state = DeviceState(self.clock, scenario or Scenario())
        self.unreachable_until_ms = None

    #: Until this clock time the device answers nothing (MCUboot is swapping).
    unreachable_until_ms: int | None = None

    def reboot(self, at_ms: int | None = None) -> None:
        """A new boot of the same device: a new `boot_id`, uptime from zero, and
        everything the device keeps in RAM gone - sessions, jobs, the log rings,
        a staged or applied transaction. What it keeps durably stays: the
        administrator password, the committed network configuration, the staged
        firmware file and the update journals - an install the reboot cut short
        is `interrupted` and does not continue.

        The STM32 slots come along too (system.py): a pending swap is performed
        and an unconfirmed new firmware is reverted, and either way the device
        answers nothing until MCUboot is done. `at_ms` is when the restart
        happened, if not now - the moment an install's `rebooting` phase ended,
        however late the next request came."""
        old = self.state
        old.settle()
        now = self.clock.now_ms()
        at = now if at_ms is None else min(at_ms, now)
        self.state = DeviceState(self.clock, old.scenario)
        self.state.auth.password = old.auth.password
        self.state.auth.setup_required = old.auth.setup_required
        self.state.auth.setup_allowed = old.auth.setup_allowed
        self.state.network.revision = old.network.revision
        self.state.network.config = old.network.config
        self.state.firmware.survive_reboot(old.firmware)
        self.state.coprocessor.survive_reboot(old.coprocessor)
        dark_ms, consumed = self.state.system.survive_reboot(old.system, at)
        upload = self.state.firmware.upload
        if consumed is not None and upload is not None and upload.id == consumed:
            self.state.firmware.upload = None
        self.unreachable_until_ms = at + dark_ms if dark_ms else None
        # The new boot starts when the application does, after the swap.
        self.state.boot_ms = at + dark_ms

    def _restart_if_due(self) -> None:
        """An install whose `rebooting` phase has ended restarts the device."""
        self.state.settle()
        system = self.state.system
        if system.reboot_due:
            self.reboot(at_ms=system.reboot_at_ms)

    def unreachable(self) -> bool:
        until = self.unreachable_until_ms
        return until is not None and self.clock.now_ms() < until

    # -- entry point -----------------------------------------------------

    def handle(self, request: Request) -> Response:
        self._restart_if_due()
        if self.unreachable() and not request.path.startswith("/__mock"):
            # No answer at all: status 0 tells the HTTP adapter to close the
            # connection without a response, which is what a browser sees while
            # the device restarts.
            return Response(0, b"", {})
        if (
            self.static is not None
            and not (request.path == "/api" or request.path.startswith("/api/"))
            and not request.path.startswith("/__mock")
        ):
            # Not the API: no request id, no no-store - the device's static
            # answers carry their own cache policy.
            return self.static.respond(request)
        request_id = next_request_id()
        try:
            self.state.settle()
            if request.path.startswith("/__mock"):
                return self._control(request, request_id)
            response = self._dispatch(request, request_id)
        except ApiError as err:
            err.request_id = request_id
            response = error_response(err)
        except Exception as unexpected:  # pragma: no cover - a mock bug, not a device one
            err = error("internal_error", f"mock failure: {type(unexpected).__name__}")
            err.request_id = request_id
            response = error_response(err)
        response.headers.setdefault("X-Request-ID", request_id)
        response.headers.setdefault("Cache-Control", "no-store")
        return response

    def _dispatch(self, request: Request, request_id: str) -> Response:
        base = self.doc.base_path
        if request.path == "/api" or request.path.startswith("/api/"):
            # Before anything else, as on the device: the setup token is
            # published by GET /auth/state, and a DNS-rebinding page whose name
            # resolves to the device would otherwise read it as same-origin.
            if not host_allowed(request.headers.get("host"), self.state.scenario.extra_hosts):
                raise error("origin_rejected", "Host is not this device")
        if not request.path.startswith(base + "/"):
            # An unknown API URL is a JSON 404 and never a redirect to the SPA,
            # which is the rule that keeps a mistyped endpoint from arriving at
            # the frontend as an HTML page.
            raise error("not_found", f"{request.path} is not an API resource")
        tail = request.path[len(base) :]

        matched_path = False
        for pattern, op in self._routes:
            match = pattern.match(tail)
            if not match:
                continue
            matched_path = True
            if op.method != request.method:
                continue
            return self._invoke(op, request, match.groupdict(), request_id)

        if matched_path:
            # No status in the contract's table means "method not allowed", and
            # the rule for an unknown API URL is a JSON 404. Reported as
            # not_found with the method named, so the message says what happened.
            raise error("not_found", f"{request.method} is not defined for {tail}")
        raise error("not_found", f"{tail} is not an API resource")

    def _invoke(
        self, op: Operation, request: Request, params: dict[str, str], request_id: str
    ) -> Response:
        handler = HANDLERS.get(op.operation_id)
        if handler is None:
            raise error("not_found", f"{op.operation_id} is not implemented by the mock")

        self._check_path_params(op, params)
        session = self._authenticate(op, request)
        self._check_origin(op, request)
        self._check_csrf(op, request, session)
        self._check_required_headers(op, request)
        body = self._parse_body(op, request)
        self._check_query(op, request)

        context = Context(
            request=request,
            operation=op,
            body=body,
            params=params,
            session=session,
            request_id=request_id,
        )

        replay = self._idempotency_lookup(op, request, body, session)
        if replay is not None:
            return replay
        response = handler(self, context)
        self._idempotency_store(op, request, body, session, response)
        return response

    # -- middleware ------------------------------------------------------

    def _check_path_params(self, op: Operation, params: dict[str, str]) -> None:
        for param in op.path_parameters:
            value = params.get(param.name, "")
            for err in self.doc.validator(param.schema).iter_errors(value):
                raise error(
                    "not_found",
                    f"{param.name} is not a valid identifier: {err.message}",
                )

    def _authenticate(self, op: Operation, request: Request) -> Any:
        if not op.requires_session:
            return None
        return self.state.auth.resolve(request.cookie(self.state.auth.COOKIE))

    def _check_origin(self, op: Operation, request: Request) -> None:
        """Origin, for the two operations that run before a CSRF token exists.

        An absent `Origin` is allowed: a browser always sends one on a
        cross-site POST, which is the case being defended against, and refusing
        its absence would only break `curl` without protecting anything.
        """
        if op.operation_id not in ORIGIN_CHECKED:
            return
        origin = request.headers.get("origin")
        if origin is None:
            return
        host = request.headers.get("host", "")
        if urlsplit(origin).netloc != host:
            raise error("origin_rejected", f"Origin {origin!r} is not this device")

    def _check_csrf(self, op: Operation, request: Request, session: Any) -> None:
        if not any(p.name.lower() == CSRF_HEADER for p in op.header_parameters):
            return
        token = request.headers.get(CSRF_HEADER)
        if session is None or token is None or token != session.csrf_token:
            raise error("csrf_failed", "The CSRF token is missing or does not match the session")

    def _check_required_headers(self, op: Operation, request: Request) -> None:
        """Required headers, validated against their declared schemas.

        DECISION. The contract does not say which code a malformed required
        header earns. `validation_failed` is used, with no `fields` entry: the
        `fields` list is a list of JSON Pointers into the request body, and
        inventing a pointer syntax for headers would put a shape in the error
        body that the document does not describe. The header is named in the
        message instead.
        """
        for param in op.header_parameters:
            name = param.name.lower()
            if name in (CSRF_HEADER,):
                continue  # handled above, with its own code
            value = request.headers.get(name)
            if value is None:
                if param.required:
                    if name == SETUP_TOKEN_HEADER:
                        raise error("setup_not_allowed", f"{param.name} is required for setup")
                    raise error("validation_failed", f"The {param.name} header is required")
                continue
            for err in self.doc.validator(param.schema).iter_errors(value):
                raise error("validation_failed", f"{param.name} is not acceptable: {err.message}")

    def _check_query(self, op: Operation, request: Request) -> None:
        declared = {p.name: p for p in op.query_parameters}
        query = request.query
        for name, values in query.items():
            param = declared.get(name)
            if param is None:
                raise error(
                    "invalid_query", f"{name!r} is not a query parameter of this operation"
                )
            if len(values) > 1:
                raise error("invalid_query", f"{name!r} was given more than once")
            value: Any = values[0]
            schema = param.schema
            if schema.get("type") == "integer":
                try:
                    value = int(value)
                except ValueError:
                    raise error("invalid_query", f"{name!r} is not an integer") from None
            for err in self.doc.validator(schema).iter_errors(value):
                raise error("invalid_query", f"{name}: {err.message}")
        for name, param in declared.items():
            if param.required and name not in query:
                raise error("invalid_query", f"{name!r} is required")

    def _parse_body(self, op: Operation, request: Request) -> Any:
        types = op.request_content_types()
        if not types:
            if request.body:
                raise error("unsupported_media_type", "This operation takes no request body")
            return None
        declared_type = request.headers.get("content-type", "").split(";")[0].strip()
        if "application/octet-stream" in types:
            if declared_type and declared_type != "application/octet-stream":
                raise error("unsupported_media_type", "This operation takes binary data")
            return request.body
        if not request.body:
            if op.request_body_required:
                raise error("invalid_json", "A JSON body is required")
            return None
        if declared_type and declared_type != "application/json":
            raise error("unsupported_media_type", f"{declared_type!r} is not application/json")
        if len(request.body) > JSON_BODY_BYTES:
            raise error("payload_too_large", f"A JSON body may not exceed {JSON_BODY_BYTES} bytes")
        parsed = parse_json_strictly(request.body)
        schema = op.request_json_schema()
        if schema is not None:
            _validate_request(self.doc, schema, parsed)
        return parsed

    # -- idempotency -----------------------------------------------------

    def _idempotency_record(self, op: Operation, request: Request, session: Any) -> str | None:
        """Where a replay is looked up: the contract's scope plus the key.

        The scope is "admin principal + method + canonical URL, including
        significant query" — deliberately not the cookie, so a replay after a
        re-login still deduplicates, which is the case the rule exists for. The
        key is part of the address rather than the whole of it: the same key used
        on a different operation is a different action and the two must not see
        each other.
        """
        key = self._idempotency_key(op, request)
        if key is None:
            return None
        digest = hashlib.sha256()
        digest.update(b"admin\0" if session is not None else b"-\0")
        digest.update(f"{request.method}\0{request.path}\0{request.query_string}\0".encode())
        digest.update(key.encode())
        return digest.hexdigest()

    def _body_digest(self, body: Any) -> str:
        if isinstance(body, bytes):
            return hashlib.sha256(body).hexdigest()
        return hashlib.sha256(
            json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest()

    def _idempotency_lookup(
        self, op: Operation, request: Request, body: Any, session: Any
    ) -> Response | None:
        record = self._idempotency_record(op, request, session)
        if record is None:
            return None
        stored = self.state.idempotency.get(record)
        if stored is None:
            return None
        body_digest, recorded = stored
        if body_digest != self._body_digest(body):
            # Same key, same operation, different content: the client changed its
            # mind under a key that already means something, and answering with
            # the first result would hide that.
            raise error(
                "idempotency_conflict",
                "This Idempotency-Key was used with a different request",
            )
        return Response(
            status=int(recorded["status"]),
            body=bytes(recorded["body"]),  # type: ignore[arg-type]
            headers=dict(recorded["headers"]),  # type: ignore[arg-type]
        )

    def _idempotency_store(
        self, op: Operation, request: Request, body: Any, session: Any, response: Response
    ) -> None:
        record = self._idempotency_record(op, request, session)
        if record is None:
            return
        # Only a result reaches here. A rejection leaves the handler as an
        # exception and is never recorded, which is what the contract wants: the
        # client is expected to fix the request and send it again under the same
        # key, and a stored refusal would lock it out of ever succeeding.
        self.state.idempotency[record] = (
            self._body_digest(body),
            {"status": response.status, "body": response.body, "headers": dict(response.headers)},
        )

    def _idempotency_key(self, op: Operation, request: Request) -> str | None:
        if op.operation_id in IDEMPOTENCY_EXEMPT:
            return None
        if not any(p.name.lower() == IDEMPOTENCY_HEADER for p in op.header_parameters):
            return None
        return request.headers.get(IDEMPOTENCY_HEADER)

    # -- the mock's own control plane ------------------------------------

    def _control(self, request: Request, request_id: str) -> Response:
        """`/__mock/*`: reset, skip time, choose a scenario, dump state.

        Namespaced so it can never collide with the device, which serves nothing
        under `/__mock`. It exists because two things a frontend has to handle
        are otherwise unreachable in a test: a 120-second confirmation timeout,
        and the paths that only a differently-behaving device produces. A
        namespace the device does not serve is the price; a frontend that came to
        depend on it would fail against the real thing immediately, which is the
        right way for that mistake to surface.
        """
        path = request.path[len("/__mock") :].strip("/")
        if request.method == "GET" and path == "state":
            body = self.state.to_json()
            until = self.unreachable_until_ms
            body["unreachable_for_ms"] = (
                max(0, until - self.clock.now_ms()) if until is not None else 0
            )
            return json_response(200, body)
        if request.method == "POST" and path == "reset":
            payload = json.loads(request.body) if request.body else {}
            scenario = Scenario()
            scenario.update(payload.get("scenario", {}))
            self.reset(scenario)
            return json_response(200, self.state.to_json())
        if request.method == "POST" and path == "advance":
            payload = json.loads(request.body) if request.body else {}
            seconds = payload.get("seconds", 0)
            if not isinstance(seconds, (int, float)) or seconds < 0:
                raise error("validation_failed", "seconds must be a non-negative number")
            self.clock.advance(int(seconds * 1000))
            self.state.settle()
            return json_response(200, {"uptime_ms": self.state.uptime_ms})
        if request.method == "POST" and path == "reboot":
            self.reboot()
            return json_response(200, self.state.to_json())
        if request.method == "POST" and path == "scenario":
            payload = json.loads(request.body) if request.body else {}
            self.state.scenario.update(payload)
            return json_response(200, {"scenario": vars(self.state.scenario)})
        raise error("not_found", f"/__mock/{path} is not a control endpoint")


# -- request parsing and validation ------------------------------------------
#
# What follows reproduces modules/web-api/lib/json_reader.c, whose header
# documents the rules. The device cannot use a general JSON library, so it
# decides every edge explicitly; the mock uses Python's, and closes each gap
# where Python would answer differently: duplicate names, NaN, nesting, the
# value of an integer, text a C string cannot hold, and the exact list of
# field errors. A frontend must not be able to tell the two apart by how
# either refuses a body.

#: WEB_JSON_MAX_DEPTH and the per-object member bound in json_reader.c.
MAX_DEPTH = 8
MAX_MEMBERS = 64


class _Refused(Exception):
    """A body that is not one well-formed JSON document by the device's rules."""


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    if len(pairs) > MAX_MEMBERS:
        raise _Refused("An object has too many members")
    names = set()
    for name, _ in pairs:
        if name in names:
            # I-JSON (RFC 7493): implementations disagree on which duplicate
            # wins, which is how one component's validated request means
            # something else to the next.
            raise _Refused("A member name appears twice in one object")
        names.add(name)
    return dict(pairs)


def _constant(name: str) -> Any:
    raise _Refused(f"{name} is not JSON")


def _number(text: str) -> Any:
    """JSON Schema's integer is a value: 120.0 and 1.2e2 are 120.

    An integral value comes back as an int, so the schema's bounds apply to it;
    a fractional one as a float, which "integer" refuses. A magnitude past what
    any bound here could allow is clamped rather than materialised - "1e99999999"
    must not build a hundred-million-digit integer - and still fails its bound,
    as it does on the device, where it does not fit int64_t.
    """
    value = decimal.Decimal(text)
    if value != value.to_integral_value():
        return float(text)
    if value.adjusted() > 19:
        return 10**20 if value > 0 else -(10**20)
    return int(value)


def _depth(value: Any) -> int:
    if isinstance(value, dict):
        return 1 + max((_depth(v) for v in value.values()), default=0)
    if isinstance(value, list):
        return 1 + max((_depth(v) for v in value), default=0)
    return 0


def parse_json_strictly(body: bytes) -> Any:
    try:
        text = body.decode("utf-8")
        parsed = json.loads(
            text, object_pairs_hook=_pairs, parse_constant=_constant, parse_float=_number
        )
    except _Refused as exc:
        raise error("invalid_json", str(exc)) from None
    except UnicodeDecodeError:
        raise error("invalid_json", "The body is not valid UTF-8") from None
    except ValueError as exc:
        raise error("invalid_json", f"The body is not valid JSON: {exc}") from None
    if _depth(parsed) > MAX_DEPTH:
        raise error("invalid_json", "The body is nested too deeply")
    return parsed


def host_allowed(host: str | None, extra_hosts: str = "") -> bool:
    """web_api_host_allowed(): an IP literal, localhost, or a configured name."""
    if host is None:
        return True
    if not host or len(host) >= 64:
        return False

    def port_ok(port: str) -> bool:
        return 1 <= len(port) <= 5 and port.isdigit() and port.isascii() and int(port) <= 65535

    if host.startswith("["):
        end = host.find("]")
        if end < 0:
            return False
        rest = host[end + 1 :]
        if rest and not (rest.startswith(":") and port_ok(rest[1:])):
            return False
        inner = host[1:end]
        if "%" in inner:
            return False  # a zone id is not an address the device parses
        try:
            ipaddress.IPv6Address(inner)
        except ValueError:
            return False
        return True

    name, colon, port = host.partition(":")
    if colon and not port_ok(port):
        return False
    try:
        ipaddress.IPv4Address(name)
        return True
    except ValueError:
        pass
    names = ["localhost"] + [n.strip() for n in extra_hosts.split(",") if n.strip()]
    return name.isascii() and any(name.lower() == n.lower() for n in names)


#: How a schema violation becomes one of `api-validation`'s closed field codes.
#: The mapping is the whole reason the mock's rejections are useful to build
#: against: a frontend can branch on the code rather than parse the message.
_FIELD_CODE_BY_KEYWORD = {
    "required": "required",
    "additionalProperties": "unknown_field",
    "type": "invalid_format",
    "pattern": "invalid_format",
    "format": "invalid_format",
    "enum": "not_allowed",
    "const": "not_allowed",
    "minimum": "out_of_range",
    "maximum": "out_of_range",
    "exclusiveMinimum": "out_of_range",
    "exclusiveMaximum": "out_of_range",
    "minLength": "out_of_range",
    "minItems": "out_of_range",
    "maxItems": "out_of_range",
    "maxLength": "too_long",
    "oneOf": "conflicting",
    "anyOf": "invalid_format",
    "allOf": "invalid_format",
}

#: When two reports name one path, the first of these wins (json_reader.c).
_PRECEDENCE = ("required", "unknown_field", "too_long", "out_of_range", "invalid_format",
               "not_allowed", "conflicting")

#: API_ERROR_PATH_MAX_LEN.
_PATH_MAX_BYTES = 63


def _pointer(parts: list[Any]) -> str:
    """RFC 6901, clipped the way the device clips: a segment that holds a
    control character, or would make the pointer longer than 63 bytes, is left
    off, and the report names the enclosing value."""
    pointer = ""
    for part in parts:
        segment = str(part)
        if any(ord(c) < 0x20 for c in segment):
            break
        candidate = pointer + "/" + segment.replace("~", "~0").replace("/", "~1")
        if len(candidate.encode()) > _PATH_MAX_BYTES:
            break
        pointer = candidate
    return pointer or "/"


def _segment_compare(a: str, b: str) -> int:
    """path_compare() in json_reader.c: segment by segment, digits as numbers."""
    sa = [] if a == "/" else a[1:].split("/")
    sb = [] if b == "/" else b[1:].split("/")
    for x, y in zip(sa, sb):
        if x.isdigit() and y.isdigit() and x.isascii() and y.isascii() and len(x) != len(y):
            return -1 if len(x) < len(y) else 1
        bx, by = x.encode(), y.encode()
        if bx != by:
            common = min(len(bx), len(by))
            if bx[:common] != by[:common]:
                return -1 if bx[:common] < by[:common] else 1
            return -1 if len(bx) < len(by) else 1
    if len(sa) != len(sb):
        return -1 if len(sa) < len(sb) else 1
    return 0


def _translate(problem: Any, found: list[tuple[list[Any], str]]) -> None:
    parts = list(problem.absolute_path)
    keyword = str(problem.validator)

    if keyword in ("anyOf", "oneOf") and problem.context:
        # A nullable value that is not null failed its one real branch; report
        # that branch's problem, as the device does for a nullable field.
        real = [
            e for e in problem.context
            if not (e.validator == "type" and e.validator_value == "null")
        ]
        branches = {e.relative_schema_path[0] for e in real}
        if real and len(branches) == 1:
            for sub in real:
                _translate(sub, found)
            return

    if keyword == "required":
        missing = str(problem.message).split("'")[1]
        found.append((parts + [missing], "required"))
        return
    if keyword == "additionalProperties":
        declared = set(problem.schema.get("properties", {}))
        for extra in problem.instance:
            if extra not in declared:
                found.append((parts + [extra], "unknown_field"))
        return
    found.append((parts, _FIELD_CODE_BY_KEYWORD.get(keyword, "invalid_format")))


def _unholdable_strings(value: Any, parts: list[Any]) -> list[list[Any]]:
    """Strings a C buffer cannot hold: U+0000, or an unpaired surrogate."""
    out: list[list[Any]] = []
    if isinstance(value, str):
        if "\x00" in value or any(0xD800 <= ord(c) <= 0xDFFF for c in value):
            out.append(parts)
    elif isinstance(value, dict):
        for key, item in value.items():
            out.extend(_unholdable_strings(item, parts + [key]))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            out.extend(_unholdable_strings(item, parts + [index]))
    return out


def _validate_request(doc: Document, schema: dict[str, Any], body: Any) -> None:
    """Reject a malformed body the way the device will.

    One entry per bad value, every undeclared member reported, entries sorted
    by pointer segment by segment - the list json_reader.c produces for the same
    body, so the same entries survive truncation on both.
    """
    validator = doc.validator(schema)
    found: list[tuple[list[Any], str]] = []
    for problem in validator.iter_errors(body):
        _translate(problem, found)

    # Text the device refuses and Python holds happily. A value that already
    # has a report, or lies inside one, keeps it.
    for parts in _unholdable_strings(body, []):
        if not any(parts[: len(p)] == p for p, _ in found):
            found.append((parts, "invalid_format"))

    if not found:
        return

    best: dict[str, str] = {}
    order: list[str] = []
    for parts, code in found:
        pointer = _pointer(parts)
        if pointer not in best:
            best[pointer] = code
            order.append(pointer)
        elif _PRECEDENCE.index(code) < _PRECEDENCE.index(best[pointer]):
            best[pointer] = code

    err = ApiError(code="validation_failed", message="The request body is not acceptable")
    for pointer in sorted(order, key=cmp_to_key(_segment_compare)):
        err.add_field(pointer, best[pointer])
    raise err
