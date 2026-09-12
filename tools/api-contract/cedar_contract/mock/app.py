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

import hashlib
import json
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

    # -- entry point -----------------------------------------------------

    def handle(self, request: Request) -> Response:
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
        try:
            parsed = json.loads(request.body)
        except (ValueError, UnicodeDecodeError) as exc:
            raise error("invalid_json", f"The body is not valid JSON: {exc}") from None
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
            return json_response(200, self.state.to_json())
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
        if request.method == "POST" and path == "scenario":
            payload = json.loads(request.body) if request.body else {}
            self.state.scenario.update(payload)
            return json_response(200, {"scenario": vars(self.state.scenario)})
        raise error("not_found", f"/__mock/{path} is not a control endpoint")


# -- request validation ----------------------------------------------------

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


def _validate_request(doc: Document, schema: dict[str, Any], body: Any) -> None:
    """Reject a malformed body the way the device will.

    The point is not to be a schema validator — `jsonschema` is that. It is to
    turn one into the contract's error body, with a JSON Pointer and a code per
    bad field, so the frontend's error handling is exercised against the shape
    it will meet rather than against a bare 400.
    """
    validator = doc.validator(schema)
    errors = sorted(validator.iter_errors(body), key=lambda e: list(e.absolute_path))
    if not errors:
        return
    err = ApiError(code="validation_failed", message="The request body is not acceptable")
    for problem in errors:
        pointer = "/" + "/".join(str(part) for part in problem.absolute_path)
        code = _FIELD_CODE_BY_KEYWORD.get(str(problem.validator), "invalid_format")
        if problem.validator == "required":
            missing = str(problem.message).split("'")[1]
            pointer = f"{pointer.rstrip('/')}/{missing}"
        elif problem.validator == "additionalProperties":
            extra = str(problem.message).split("'")[1]
            pointer = f"{pointer.rstrip('/')}/{extra}"
        err.add_field(pointer if pointer != "/" else "/", code)
    raise err
