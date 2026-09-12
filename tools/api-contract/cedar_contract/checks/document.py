# SPDX-License-Identifier: Apache-2.0
"""The document checked against itself.

Every check returns findings rather than raising, so one run reports everything
wrong with the document instead of the first thing. Each finding carries the
JSON Pointer to the offending node: a contract document is 4700 lines, and
"examples are invalid" without a location is not actionable.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from jsonschema.exceptions import ValidationError

from ..openapi import HTTP_METHODS, Document

#: The plan's fifth check, and why it is not here yet. Kept as data so the
#: report prints it: a deferred check that leaves no trace becomes a forgotten
#: one, and this is the reason it does not appear in the P1 results.
DEFERRED: dict[str, str] = {
    "undocumented_routes": (
        "Nothing to compare against yet. Detecting a route the device serves "
        "but the document does not declare needs the web-api route table, and "
        "web-api does not exist before P2. The mock is built so the comparison "
        "is cheap when it does: it routes off this document, so the device's "
        "table can be diffed against the same index. Revisit in P2."
    ),
}


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


#: Name -> check, in the order the plan lists them.
CHECKS: dict[str, Callable[[Document], list[Finding]]] = {
    "refs_resolve": check_refs_resolve,
    "path_parameters": check_path_parameters,
    "operation_ids": check_operation_ids,
    "examples_valid": check_examples_valid,
}


def run_all(doc: Document) -> list[Finding]:
    findings: list[Finding] = []
    for check in CHECKS.values():
        findings.extend(check(doc))
    return findings
