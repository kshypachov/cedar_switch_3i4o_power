# SPDX-License-Identifier: Apache-2.0
"""The document checked against itself.

Every check returns findings rather than raising, so one run reports everything
wrong with the document instead of the first thing. Each finding carries the
JSON Pointer to the offending node: a contract document is 4700 lines, and
"examples are invalid" without a location is not actionable.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from jsonschema.exceptions import ValidationError

from ..openapi import HTTP_METHODS, Document

#: Checks the plan lists that cannot run yet, with the reason. Empty since P2:
#: the fifth check compares the device's route table with this document.
DEFERRED: dict[str, str] = {}

#: The device's route table and the server resources it is registered under.
REPO = Path(__file__).resolve().parents[4]
ROUTES_FILE = REPO / "src" / "web" / "api" / "v1" / "routes.h"
RESOURCES_FILE = REPO / "src" / "web" / "api" / "v1" / "http_resources.h"


@dataclass(frozen=True)
class Finding:
    """One problem, located."""

    check: str
    pointer: str
    message: str

    def __str__(self) -> str:
        return f"{self.check}: {self.pointer}: {self.message}"


def check_refs_resolve(doc: Document) -> list[Finding]:
    """Every `$ref` is internal and lands on a node that exists.

    External references are refused rather than followed. The document is meant
    to be self-contained — the frontend generates types from this one file, and
    a reference out of it would make the build depend on the network.
    """
    findings: list[Finding] = []
    for pointer, ref in doc.iter_refs():
        if not ref.startswith("#/"):
            findings.append(
                Finding("refs_resolve", pointer, f"reference is not internal: {ref!r}")
            )
            continue
        try:
            doc.resolve(ref)
        except (KeyError, IndexError, ValueError):
            findings.append(Finding("refs_resolve", pointer, f"{ref} resolves to nothing"))
    return findings


def check_path_parameters(doc: Document) -> list[Finding]:
    """Path templates and declared path parameters agree, in both directions.

    A placeholder with no declaration leaves the generated client with an
    untyped hole; a declaration with no placeholder is dead weight that reads
    as a real parameter. OpenAPI also requires `required: true` on a path
    parameter, and a client generator that trusts `required` would emit an
    optional argument for something the URL cannot omit.
    """
    findings: list[Finding] = []
    for op in doc.operations:
        where = op.pointer
        declared = {p.name: p for p in op.path_parameters}
        in_template = set(op.template_parameters)
        for name in sorted(in_template - set(declared)):
            findings.append(
                Finding("path_parameters", where, f"{{{name}}} in the path is not declared")
            )
        for name in sorted(set(declared) - in_template):
            findings.append(
                Finding(
                    "path_parameters",
                    where,
                    f"parameter {name!r} is declared in:path but absent from {op.path!r}",
                )
            )
        for name, param in sorted(declared.items()):
            if not param.required:
                findings.append(
                    Finding("path_parameters", where, f"path parameter {name!r} is not required")
                )
            if not param.schema:
                findings.append(
                    Finding("path_parameters", where, f"path parameter {name!r} has no schema")
                )
        if len(op.template_parameters) != len(in_template):
            findings.append(
                Finding("path_parameters", where, f"{op.path!r} repeats a placeholder")
            )
    return findings


def check_operation_ids(doc: Document) -> list[Finding]:
    """Every operation has an `operationId`, and no two share one.

    The generated client names its methods from these, so a duplicate silently
    drops an operation from the frontend's API surface, and a missing one makes
    the generator invent a name that changes the next time a path does.
    """
    findings: list[Finding] = []
    seen: dict[str, str] = {}
    for path, item in doc.raw.get("paths", {}).items():
        for method, op in item.items():
            if method not in HTTP_METHODS:
                continue
            where = f"#/paths/{path}/{method}"
            op_id = op.get("operationId")
            if not op_id:
                findings.append(Finding("operation_ids", where, "no operationId"))
                continue
            if op_id in seen:
                findings.append(
                    Finding("operation_ids", where, f"operationId {op_id!r} also on {seen[op_id]}")
                )
            else:
                seen[op_id] = where
    return findings


def check_examples_valid(doc: Document) -> list[Finding]:
    """Every example validates against the schema it illustrates.

    Examples are what the frontend's fixtures are built from, so an example
    that does not satisfy its own schema produces a test suite that passes
    against data the device will never send.
    """
    findings: list[Finding] = []
    for pointer, schema, value in doc.iter_examples():
        validator = doc.validator(schema)
        for error in sorted(validator.iter_errors(value), key=lambda e: list(e.absolute_path)):
            path, message = _deepest(error)
            location = "/".join(str(part) for part in path)
            findings.append(
                Finding(
                    "examples_valid",
                    f"{pointer}/{location}" if location else pointer,
                    message,
                )
            )
    return findings


def _deepest(error: ValidationError) -> tuple[list[object], str]:
    """Descend through `anyOf`/`oneOf` to the sub-error that actually explains it.

    Most nullable fields in this document are `anyOf: [<schema>, null]`, and a
    bad value inside one reports only "is not valid under any of the given
    schemas" at the field. That names the field but not the problem, which is
    the difference between a finding someone can fix and one they have to
    bisect.

    The branch chosen is the one that failed *deepest* in the instance, not
    `jsonschema`'s `best_match`: for a nullable object, `best_match` prefers the
    `type: null` branch, whose complaint is that the object is not null — true
    and useless. Where every branch fails at the same depth, no branch is more
    informative than the others and the parent's own message is kept.
    """
    path = list(error.absolute_path)
    message = error.message
    while error.context:
        deeper = max(error.context, key=lambda e: len(e.absolute_path))
        if len(deeper.absolute_path) <= len(path):
            break
        error = deeper
        path = list(error.absolute_path)
        message = error.message
    return path, message


# -- the fifth check: what the device serves ------------------------------------

_ROUTE_LINE = re.compile(
    r"^WEB_API_V1_ROUTE\(\s*(?P<op>\w+)\s*,\s*(?P<method>GET|POST|PUT|DELETE)\s*,"
    r"\s*\"(?P<path>[^\"]+)\"\s*,\s*(?P<flags>[^,]+?)\s*,\s*(?P<size>[^,]+?)\s*,"
    r"\s*(?P<schema>[^,]+?)\s*,\s*(?P<query>[^,]+?)\s*,\s*(?P<handler>\w+)\s*\)\s*$"
)
_RESOURCE_LINE = re.compile(r"^WEB_API_V1_RESOURCE\(\s*(\w+)\s*,\s*\"([^\"]+)\"\s*\)\s*$")

#: web-api flag -> the header parameter the document declares for it.
_HEADER_FLAGS = {
    "WEB_API_CSRF": "x-csrf-token",
    "WEB_API_IDEMPOTENT": "idempotency-key",
    "WEB_API_SETUP_TOKEN": "x-setup-token",
}


@dataclass(frozen=True)
class Route:
    """One line of routes.h, as the check reads it."""

    operation_id: str
    method: str
    path: str
    flags: frozenset[str]
    has_body: bool
    has_query: bool
    line: int


def parse_routes(text: str, name: str = "routes.h") -> tuple[list[Route], list[Finding]]:
    routes: list[Route] = []
    findings: list[Finding] = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line.startswith("WEB_API_V1_ROUTE("):
            continue
        m = _ROUTE_LINE.match(line)
        if not m:
            findings.append(
                Finding("undocumented_routes", f"{name}:{number}", "a route line this check cannot read")
            )
            continue
        flags = frozenset(f.strip() for f in m["flags"].split("|") if f.strip() not in ("0", ""))
        routes.append(
            Route(
                operation_id=m["op"],
                method=m["method"].lower(),
                path=m["path"],
                flags=flags,
                has_body=m["schema"] != "NULL",
                has_query=m["query"] != "NULL",
                line=number,
            )
        )
    return routes, findings


def served_operation_ids(routes_path: Path = ROUTES_FILE) -> set[str]:
    routes, _ = parse_routes(routes_path.read_text(), routes_path.name)
    return {r.operation_id for r in routes}


def check_undocumented_routes(
    doc: Document, routes_path: Path = ROUTES_FILE, resources_path: Path = RESOURCES_FILE
) -> list[Finding]:
    """The device serves nothing the document does not declare, the way it declares it.

    Read from the route table itself (src/web/api/v1/routes.h), which is the
    list web-api dispatches from, so what is checked is what runs. Every route
    must be a declared operation with the same method and path, and the flags
    that are facts of the document must agree with it: a public route is one
    with `security: []`, a CSRF, idempotency or setup-token route is one that
    declares that header, a route that decodes a body is one with a request
    body, `required` included, and one that accepts query parameters declares
    some. `WEB_API_ORIGIN` is not checked: which operations run before a CSRF
    token exists is the contract's prose, not a field of the document.

    Every distinct path must also have its server resource
    (http_resources.h), and no resource may be left without a route - the
    firmware's matching itself is exercised in tests/web_api_http.

    Operations the document declares and the device does not serve are not
    findings: each belongs to a later stage. The command prints them.
    """
    name = routes_path.name
    routes, findings = parse_routes(routes_path.read_text(), name)
    by_id = {op.operation_id: op for op in doc.operations}

    seen_ids: set[str] = set()
    seen_endpoints: set[tuple[str, str]] = set()
    for route in routes:
        where = f"{name}:{route.line}"
        if route.operation_id in seen_ids:
            findings.append(Finding("undocumented_routes", where, f"{route.operation_id} is routed twice"))
        seen_ids.add(route.operation_id)
        if (route.method, route.path) in seen_endpoints:
            findings.append(
                Finding("undocumented_routes", where, f"{route.method.upper()} {route.path} is routed twice")
            )
        seen_endpoints.add((route.method, route.path))

        op = by_id.get(route.operation_id)
        if op is None:
            findings.append(
                Finding(
                    "undocumented_routes",
                    where,
                    f"serves {route.method.upper()} {route.path} as {route.operation_id}, "
                    "which the document does not declare",
                )
            )
            continue
        if (op.method.lower(), op.path) != (route.method, route.path):
            findings.append(
                Finding(
                    "undocumented_routes",
                    where,
                    f"{route.operation_id} is {op.method.upper()} {op.path} in the document, "
                    f"routed as {route.method.upper()} {route.path}",
                )
            )

        public = "WEB_API_PUBLIC" in route.flags
        if public == op.requires_session:
            findings.append(
                Finding(
                    "undocumented_routes",
                    where,
                    f"{route.operation_id}: "
                    + ("routed public, but the document requires a session"
                       if public else "routed with a session, but the document declares security: []"),
                )
            )
        headers = {p.name.lower() for p in op.header_parameters}
        for flag, header in _HEADER_FLAGS.items():
            if (flag in route.flags) != (header in headers):
                findings.append(
                    Finding(
                        "undocumented_routes",
                        where,
                        f"{route.operation_id}: {flag} "
                        + ("set" if flag in route.flags else "missing")
                        + f", document {'declares' if header in headers else 'does not declare'} {header}",
                    )
                )
        if route.has_body != (op.request_body is not None):
            findings.append(
                Finding(
                    "undocumented_routes",
                    where,
                    f"{route.operation_id}: "
                    + ("decodes a body the document does not declare" if route.has_body
                       else "declares no body schema, but the document has a request body"),
                )
            )
        if ("WEB_API_BODY_REQUIRED" in route.flags) != (
            op.request_body is not None and op.request_body_required
        ):
            findings.append(
                Finding(
                    "undocumented_routes",
                    where,
                    f"{route.operation_id}: WEB_API_BODY_REQUIRED disagrees with requestBody.required",
                )
            )
        if route.has_query != bool(op.query_parameters):
            findings.append(
                Finding(
                    "undocumented_routes",
                    where,
                    f"{route.operation_id}: query parameters "
                    + ("accepted but not declared" if route.has_query else "declared but not accepted"),
                )
            )

    resources: dict[str, int] = {}
    for number, raw in enumerate(resources_path.read_text().splitlines(), start=1):
        line = raw.strip()
        if not line.startswith("WEB_API_V1_RESOURCE("):
            continue
        m = _RESOURCE_LINE.match(line)
        if not m:
            findings.append(
                Finding("undocumented_routes", f"{resources_path.name}:{number}", "a resource line this check cannot read")
            )
            continue
        resources[m.group(2)] = number
    wanted = {doc.base_path + re.sub(r"\{[^}]+\}", "*", r.path) for r in routes}
    for pattern in sorted(wanted - set(resources)):
        findings.append(
            Finding("undocumented_routes", resources_path.name, f"no server resource for {pattern}")
        )
    for pattern in sorted(set(resources) - wanted):
        findings.append(
            Finding(
                "undocumented_routes",
                f"{resources_path.name}:{resources[pattern]}",
                f"resource {pattern} has no route",
            )
        )
    return findings


#: Name -> check, in the order the plan lists them.
CHECKS: dict[str, Callable[[Document], list[Finding]]] = {
    "refs_resolve": check_refs_resolve,
    "path_parameters": check_path_parameters,
    "operation_ids": check_operation_ids,
    "examples_valid": check_examples_valid,
    "undocumented_routes": check_undocumented_routes,
}


def run_all(doc: Document) -> list[Finding]:
    findings: list[Finding] = []
    for check in CHECKS.values():
        findings.extend(check(doc))
    return findings
