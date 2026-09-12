# SPDX-License-Identifier: Apache-2.0
"""`openapi.json` read as the source of truth it is declared to be.

The contract says the document is authoritative for formats. Two things follow,
and both are the reason this module exists rather than each consumer reading
the JSON its own way:

- **The mock routes off the document, not off a hand-written table.** An
  operation the document declares and the mock does not answer is a gap the
  coverage test reports; a handler for an operation the document does not
  declare cannot even be registered. The middleware that checks required
  headers, content types and request schemas also reads the declaration, so the
  mock enforces what the document says rather than what its author remembered.
- **Validation is real schema validation.** `jsonschema` is already a hard
  dependency of the toolchain — it is in `zephyr/scripts/requirements-base.txt`
  and installed in `tests/ci/Dockerfile` — so using it adds nothing to install,
  and hand-rolling a 2020-12 validator would produce something less
  trustworthy than the thing it replaced.

`format` needed a decision. `jsonschema`'s default checker covers `ipv4` and
`ipv6` from the standard library but leaves `date-time` and `uri` unchecked
unless two more packages are installed. Rather than add dependencies for two
formats, both are registered here against the standard library. An unchecked
format in a contract test is a silent hole: `wall_time` and `reconnect_urls`
are exactly the fields a careless mock would fill with something a browser
cannot parse.
"""

from __future__ import annotations

import datetime
import json
import re
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from jsonschema import Draft202012Validator, FormatChecker
from referencing import Registry, Resource
from referencing.jsonschema import DRAFT202012

#: Repository-relative location of the document, resolved from this file.
DEFAULT_DOCUMENT = (
    Path(__file__).resolve().parents[3] / "docs" / "device-development" / "openapi.json"
)

#: Base URI the document is registered under so `#/components/...` resolves.
_BASE_URI = "urn:cedar-openapi"

HTTP_METHODS = ("get", "put", "post", "delete", "patch", "head", "options", "trace")

_TEMPLATE_RE = re.compile(r"\{([^{}]+)\}")


def _build_format_checker() -> FormatChecker:
    checker = FormatChecker()

    @checker.checks("date-time", raises=ValueError)
    def _date_time(value: object) -> bool:
        if not isinstance(value, str):
            return True
        # RFC 3339 as the contract uses it. `fromisoformat` accepts the whole
        # grammar from Python 3.11 on, including the trailing Z.
        datetime.datetime.fromisoformat(value)
        return True

    @checker.checks("uri", raises=ValueError)
    def _uri(value: object) -> bool:
        if not isinstance(value, str):
            return True
        parts = urlsplit(value)
        # Absolute only: a reconnect hint the browser cannot navigate to is
        # worse than no hint, and the schema asks for a URI, not a reference.
        return bool(parts.scheme) and bool(parts.netloc)

    return checker


FORMAT_CHECKER = _build_format_checker()


@dataclass(frozen=True)
class Parameter:
    name: str
    location: str
    required: bool
    schema: dict[str, Any]


@dataclass(frozen=True)
class Operation:
    """One declared operation, with everything a router or a check needs."""

    operation_id: str
    method: str
    path: str
    parameters: tuple[Parameter, ...]
    request_body: dict[str, Any] | None
    request_body_required: bool
    responses: dict[str, dict[str, Any]]
    security: tuple[dict[str, Any], ...]
    pointer: str

    @property
    def path_parameters(self) -> tuple[Parameter, ...]:
        return tuple(p for p in self.parameters if p.location == "path")

    @property
    def header_parameters(self) -> tuple[Parameter, ...]:
        return tuple(p for p in self.parameters if p.location == "header")

    @property
    def query_parameters(self) -> tuple[Parameter, ...]:
        return tuple(p for p in self.parameters if p.location == "query")

    @property
    def template_parameters(self) -> tuple[str, ...]:
        return tuple(_TEMPLATE_RE.findall(self.path))

    @property
    def requires_session(self) -> bool:
        """An operation with `security: []` is public; anything else inherits
        the document's `adminSession` requirement."""
        return bool(self.security)

    @property
    def success_status(self) -> str:
        """The one non-`default` response status. Every operation declares
        exactly one, which is what makes a router able to answer from the
        document."""
        codes = [c for c in self.responses if c != "default"]
        if len(codes) != 1:
            raise ValueError(f"{self.operation_id} declares {codes}, expected one success")
        return codes[0]

    def success_schema(self) -> dict[str, Any] | None:
        content = self.responses[self.success_status].get("content")
        if not content:
            return None
        media = content.get("application/json")
        return media.get("schema") if media else None

    def success_content_type(self) -> str | None:
        content = self.responses[self.success_status].get("content")
        return next(iter(content)) if content else None

    def request_json_schema(self) -> dict[str, Any] | None:
        if not self.request_body:
            return None
        media = self.request_body.get("content", {}).get("application/json")
        return media.get("schema") if media else None

    def request_content_types(self) -> tuple[str, ...]:
        if not self.request_body:
            return ()
        return tuple(self.request_body.get("content", {}))


@dataclass
class Document:
    """The loaded document plus the indexes everything else here needs."""

    raw: dict[str, Any]
    source: Path | None = None
    operations: tuple[Operation, ...] = field(default_factory=tuple)

    def __post_init__(self) -> None:
        self._registry = Registry().with_resource(
            uri=_BASE_URI, resource=Resource(contents=self.raw, specification=DRAFT202012)
        )
        self._validators: dict[str, Draft202012Validator] = {}
        if not self.operations:
            self.operations = tuple(self._index_operations())

    # -- loading ---------------------------------------------------------

    @classmethod
    def load(cls, path: str | Path | None = None) -> Document:
        resolved = Path(path) if path is not None else DEFAULT_DOCUMENT
        with resolved.open(encoding="utf-8") as handle:
            return cls(raw=json.load(handle), source=resolved)

    @classmethod
    def from_dict(cls, raw: dict[str, Any]) -> Document:
        """For the checks' own tests, which mutate a copy of the document to
        prove a check fails when it should."""
        return cls(raw=raw, source=None)

    def _index_operations(self) -> Iterator[Operation]:
        for path, item in self.raw.get("paths", {}).items():
            shared = tuple(self._parameters(item.get("parameters", [])))
            for method, op in item.items():
                if method not in HTTP_METHODS:
                    continue
                yield Operation(
                    operation_id=op.get("operationId", ""),
                    method=method.upper(),
                    path=path,
                    parameters=shared + tuple(self._parameters(op.get("parameters", []))),
                    request_body=op.get("requestBody"),
                    request_body_required=bool((op.get("requestBody") or {}).get("required")),
                    responses=op.get("responses", {}),
                    security=tuple(op.get("security", self.raw.get("security", []))),
                    pointer=f"#/paths/{path.replace('~', '~0').replace('/', '~1')}/{method}",
                )

    def _parameters(self, declared: list[dict[str, Any]]) -> Iterator[Parameter]:
        for item in declared:
            yield Parameter(
                name=item["name"],
                location=item["in"],
                required=bool(item.get("required", False)),
                schema=item.get("schema", {}),
            )

    # -- accessors -------------------------------------------------------

    @property
    def base_path(self) -> str:
        servers = self.raw.get("servers") or [{"url": "/"}]
        return servers[0]["url"].rstrip("/")

    @property
    def schemas(self) -> dict[str, Any]:
        return self.raw.get("components", {}).get("schemas", {})

    def operation(self, operation_id: str) -> Operation:
        for op in self.operations:
            if op.operation_id == operation_id:
                return op
        raise KeyError(f"no operation {operation_id!r} in {self.source or 'document'}")

    def operation_ids(self) -> tuple[str, ...]:
        return tuple(op.operation_id for op in self.operations)

    # -- validation ------------------------------------------------------

    def validator(self, schema: dict[str, Any] | str) -> Draft202012Validator:
        """A validator for a `$ref` object, a schema name, or an inline schema.

        Cached by pointer: building one per response would dominate the runtime
        of a test that walks every operation.
        """
        if isinstance(schema, str):
            pointer = f"/components/schemas/{schema}"
        elif set(schema) == {"$ref"} and schema["$ref"].startswith("#/"):
            pointer = schema["$ref"][1:]
        else:
            return Draft202012Validator(
                schema, registry=self._registry, format_checker=FORMAT_CHECKER
            )
        cached = self._validators.get(pointer)
        if cached is None:
            cached = Draft202012Validator(
                {"$ref": f"{_BASE_URI}#{pointer}"},
                registry=self._registry,
                format_checker=FORMAT_CHECKER,
            )
            self._validators[pointer] = cached
        return cached

    def resolve(self, ref: str) -> Any:
        """Follow an internal `$ref`, raising `KeyError` if it goes nowhere."""
        if not ref.startswith("#/"):
            raise KeyError(f"not an internal reference: {ref!r}")
        node: Any = self.raw
        for token in ref[2:].split("/"):
            token = token.replace("~1", "/").replace("~0", "~")
            if isinstance(node, list):
                node = node[int(token)]
            elif isinstance(node, dict) and token in node:
                node = node[token]
            else:
                raise KeyError(ref)
        return node

    def iter_refs(self) -> Iterator[tuple[str, str]]:
        """Every `$ref` in the document as (JSON Pointer to it, its value)."""

        def walk(node: Any, pointer: str) -> Iterator[tuple[str, str]]:
            if isinstance(node, dict):
                for key, value in node.items():
                    escaped = key.replace("~", "~0").replace("/", "~1")
                    if key == "$ref" and isinstance(value, str):
                        yield pointer, value
                    else:
                        yield from walk(value, f"{pointer}/{escaped}")
            elif isinstance(node, list):
                for index, value in enumerate(node):
                    yield from walk(value, f"{pointer}/{index}")

        yield from walk(self.raw, "#")

    def iter_examples(self) -> Iterator[tuple[str, dict[str, Any], Any]]:
        """Every example with the schema it must satisfy.

        Yields (pointer to the example, schema to validate against, value).
        Only the forms the document actually uses are recognised, and
        `tests/test_document_checks.py` pins that: a form added later must be
        taught here rather than silently going unchecked.
        """
        for name, schema in self.schemas.items():
            examples = schema.get("examples")
            if isinstance(examples, list):
                for index, value in enumerate(examples):
                    yield (
                        f"#/components/schemas/{name}/examples/{index}",
                        {"$ref": f"#/components/schemas/{name}"},
                        value,
                    )
            if "example" in schema:
                yield (
                    f"#/components/schemas/{name}/example",
                    {"$ref": f"#/components/schemas/{name}"},
                    schema["example"],
                )
        for _path, item in self.raw.get("paths", {}).items():
            for method, op in item.items():
                if method not in HTTP_METHODS:
                    continue
                for where, holder in self._example_holders(op):
                    schema = holder.get("schema")
                    if schema is None:
                        continue
                    if "example" in holder:
                        yield (f"{where}/example", schema, holder["example"])
                    for key, entry in (holder.get("examples") or {}).items():
                        if isinstance(entry, dict) and "value" in entry:
                            yield (f"{where}/examples/{key}", schema, entry["value"])

    def _example_holders(self, op: dict[str, Any]) -> Iterator[tuple[str, dict[str, Any]]]:
        op_id = op.get("operationId", "?")
        body = op.get("requestBody") or {}
        for media, holder in (body.get("content") or {}).items():
            yield f"#/{op_id}/requestBody/{media}", holder
        for status, response in (op.get("responses") or {}).items():
            for media, holder in (response.get("content") or {}).items():
                yield f"#/{op_id}/responses/{status}/{media}", holder


def path_to_regex(template: str) -> re.Pattern[str]:
    """`/jobs/{job_id}` -> a regex with a named group per parameter.

    A path parameter never spans a `/`: the contract's ids all match
    `^[A-Za-z0-9_-]{1,64}$`, and letting one swallow a slash would make
    `/network/transactions/{id}/apply` ambiguous with a transaction id
    containing a slash.
    """
    pattern = ["^"]
    index = 0
    for match in _TEMPLATE_RE.finditer(template):
        pattern.append(re.escape(template[index : match.start()]))
        pattern.append(f"(?P<{match.group(1)}>[^/]+)")
        index = match.end()
    pattern.append(re.escape(template[index:]))
    pattern.append("$")
    return re.compile("".join(pattern))
