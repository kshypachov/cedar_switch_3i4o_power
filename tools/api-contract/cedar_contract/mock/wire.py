# SPDX-License-Identifier: Apache-2.0
"""The values a request and a response are, and the handler registry.

Separate from `app.py` for one reason: a handler needs these and `app.py` needs
the handlers, and a package where the router imports the handlers that import
the router is a package whose import order is a puzzle. This module is the
bottom of that stack and imports nothing from the mock but its error table.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import parse_qs, urlsplit

from ..errors import ApiError, serialise
from ..openapi import Operation

#: Headers the contract names, and the cookie that carries the session.
CSRF_HEADER = "x-csrf-token"
IDEMPOTENCY_HEADER = "idempotency-key"
SETUP_TOKEN_HEADER = "x-setup-token"


@dataclass
class Request:
    method: str
    path: str
    headers: dict[str, str] = field(default_factory=dict)
    body: bytes = b""

    def __post_init__(self) -> None:
        self.headers = {k.lower(): v for k, v in self.headers.items()}
        split = urlsplit(self.path)
        self.path = split.path
        self.query_string = split.query

    @property
    def query(self) -> dict[str, list[str]]:
        return parse_qs(self.query_string, keep_blank_values=True)

    def cookie(self, name: str) -> str | None:
        raw = self.headers.get("cookie", "")
        for part in raw.split(";"):
            key, _, value = part.strip().partition("=")
            if key == name:
                return value
        return None


@dataclass
class Response:
    status: int
    body: bytes = b""
    headers: dict[str, str] = field(default_factory=dict)

    @property
    def json(self) -> Any:
        return json.loads(self.body) if self.body else None


@dataclass
class Context:
    """What a handler is given: the parsed request and the resolved session."""

    request: Request
    operation: Operation
    body: Any
    params: dict[str, str]
    session: Any
    request_id: str

    def query_one(self, name: str, default: str | None = None) -> str | None:
        values = self.request.query.get(name)
        return values[-1] if values else default


Handler = Callable[[Any, "Context"], "Response"]

#: Populated by the decorator below, keyed by `operationId`. A registry rather
#: than a table written beside the router, so a handler sits next to the state
#: machine it drives and the router has no second list to keep in step.
HANDLERS: dict[str, Handler] = {}


def handles(operation_id: str) -> Callable[[Handler], Handler]:
    def register(fn: Handler) -> Handler:
        if operation_id in HANDLERS:
            raise RuntimeError(f"two handlers for {operation_id}")
        HANDLERS[operation_id] = fn
        return fn

    return register


# -- response helpers ------------------------------------------------------


def json_response(status: int, payload: Any, headers: dict[str, str] | None = None) -> Response:
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
    merged = {"Content-Type": "application/json"}
    merged.update(headers or {})
    return Response(status=status, body=body, headers=merged)


def error_response(err: ApiError) -> Response:
    headers = {"Content-Type": "application/json"}
    if err.retry_after_seconds is not None:
        headers["Retry-After"] = str(err.retry_after_seconds)
    return Response(status=err.status, body=serialise(err), headers=headers)


def accepted(job_id: str, resource_url: str | None) -> Response:
    """The `202` body every asynchronous operation shares, with the headers the
    document declares: `Location` at the job and `Retry-After` at the contract's
    fastest sensible poll."""
    job_url = f"/api/v1/jobs/{job_id}"
    return json_response(
        202,
        {"job_id": job_id, "job_url": job_url, "resource_url": resource_url},
        {"Location": job_url, "Retry-After": "1"},
    )
