# SPDX-License-Identifier: Apache-2.0
"""The document checks, and proof that each one fails when it should.

A check that passes on a correct document has demonstrated nothing. Every check
here is therefore tested twice: once against `openapi.json` as it stands, and
once against a copy with one thing broken — a dangling `$ref`, an undeclared
path parameter, a duplicated `operationId`, an example that violates its own
schema. The second half is the half that matters, because the day the check
earns its keep is the day someone breaks the document.
"""

from __future__ import annotations

import copy
import json
from typing import Any

import pytest
from cedar_contract.checks import document as checks
from cedar_contract.openapi import Document

# -- the document as it stands --------------------------------------------


def test_the_document_passes_every_check(document: Document) -> None:
    findings = checks.run_all(document)
    assert findings == [], "\n".join(str(f) for f in findings)


def test_the_document_has_the_surface_the_plan_describes(document: Document) -> None:
    """Pinned, so that adding an operation is a deliberate act that updates the
    plan rather than a number nobody notices changing."""
    assert len(document.operations) == 35
    assert len(document.raw["paths"]) == 29
    assert len(document.schemas) == 48


def test_the_fifth_check_is_recorded_as_deferred_with_a_reason() -> None:
    """Section 12 lists five checks. The one that cannot run yet is named, so a
    reader of the P1 result knows why four are reported instead of five."""
    assert set(checks.DEFERRED) == {"undocumented_routes"}
    reason = checks.DEFERRED["undocumented_routes"]
    assert "web-api" in reason and "P2" in reason


# -- each check fails on a broken copy ------------------------------------


@pytest.fixture
def broken(document: Document) -> Any:
    """A deep copy of the real document, to be damaged by one test."""
    return copy.deepcopy(document.raw)


def _checked(raw: dict[str, Any], check: str) -> list[checks.Finding]:
    return checks.CHECKS[check](Document.from_dict(raw))


def test_refs_resolve_catches_a_dangling_reference(broken: Any) -> None:
    broken["paths"]["/auth/state"]["get"]["responses"]["200"]["content"]["application/json"][
        "schema"
    ] = {"$ref": "#/components/schemas/AuthStates"}
    findings = _checked(broken, "refs_resolve")
    assert len(findings) == 1
    assert "resolves to nothing" in findings[0].message


def test_refs_resolve_catches_an_external_reference(broken: Any) -> None:
    """Refused rather than followed: the frontend generates its types from this
    one file, and a reference out of it would make that build need the network."""
    broken["components"]["schemas"]["Session"] = {"$ref": "https://example.invalid/Session.json"}
    findings = _checked(broken, "refs_resolve")
    assert len(findings) == 1
    assert "not internal" in findings[0].message


def test_path_parameters_catches_an_undeclared_placeholder(broken: Any) -> None:
    broken["paths"]["/jobs/{job_id}"]["get"]["parameters"] = []
    findings = _checked(broken, "path_parameters")
    assert [f.message for f in findings] == ["{job_id} in the path is not declared"]


def test_path_parameters_catches_a_declaration_with_no_placeholder(broken: Any) -> None:
    broken["paths"]["/auth/state"]["get"]["parameters"] = [
        {"name": "tenant", "in": "path", "required": True, "schema": {"type": "string"}}
    ]
    findings = _checked(broken, "path_parameters")
    assert len(findings) == 1
    assert "absent from" in findings[0].message


def test_path_parameters_catches_an_optional_path_parameter(broken: Any) -> None:
    """OpenAPI requires `required: true` here, and a generator that trusts the
    flag would emit an optional argument for something the URL cannot omit."""
    broken["paths"]["/jobs/{job_id}"]["get"]["parameters"][0]["required"] = False
    findings = _checked(broken, "path_parameters")
    assert [f.message for f in findings] == ["path parameter 'job_id' is not required"]


def test_path_parameters_catches_a_parameter_with_no_schema(broken: Any) -> None:
    del broken["paths"]["/jobs/{job_id}"]["get"]["parameters"][0]["schema"]
    findings = _checked(broken, "path_parameters")
    assert [f.message for f in findings] == ["path parameter 'job_id' has no schema"]


def test_operation_ids_catches_a_duplicate(broken: Any) -> None:
    """This is the check that already passed by hand. It exists for the day
    someone copies an operation and forgets the id."""
    broken["paths"]["/matter/status"]["get"]["operationId"] = "getSystemStatus"
    findings = _checked(broken, "operation_ids")
    assert len(findings) == 1
    assert "also on" in findings[0].message


def test_operation_ids_catches_a_missing_id(broken: Any) -> None:
    del broken["paths"]["/capabilities"]["get"]["operationId"]
    findings = _checked(broken, "operation_ids")
    assert [f.message for f in findings] == ["no operationId"]


def test_examples_valid_catches_an_example_that_breaks_its_schema(broken: Any) -> None:
    broken["components"]["schemas"]["Job"]["examples"][0]["state"] = "almost_done"
    findings = _checked(broken, "examples_valid")
    assert len(findings) == 1
    assert findings[0].pointer.endswith("/state")


def test_examples_valid_catches_an_example_with_an_unknown_field(broken: Any) -> None:
    """Every response schema is `additionalProperties: false`, so an example with
    a field the schema does not have is a fixture the device would reject."""
    broken["components"]["schemas"]["JobAccepted"]["examples"][0]["retry_after"] = 1
    findings = _checked(broken, "examples_valid")
    assert len(findings) == 1
    assert "retry_after" in findings[0].message


def test_examples_valid_reaches_examples_beside_a_media_type(broken: Any) -> None:
    """The document does not use this form today. It is checked anyway, because
    the next person to add an example will reach for the OpenAPI spelling rather
    than the JSON Schema one, and an unchecked example is worse than none."""
    media = broken["paths"]["/auth/session"]["post"]["requestBody"]["content"]["application/json"]
    media["examples"] = {"wrong": {"value": {"password": 12}}}
    findings = _checked(broken, "examples_valid")
    assert len(findings) == 1
    assert "12 is not of type" in findings[0].message


def test_examples_valid_checks_declared_formats(broken: Any) -> None:
    """`ipv4`, `date-time` and `uri` are registered deliberately, because
    `jsonschema` leaves the last two unchecked without extra packages and an
    unchecked format in a contract test is a silent hole."""
    schema = broken["components"]["schemas"]["Address"]
    schema["properties"]["address"]["format"] = "ipv4"
    schema["examples"] = [
        {"family": "ipv4", "address": "192.168.1", "prefix_length": 24, "source": "dhcp"}
    ]
    assert len(_checked(broken, "examples_valid")) == 1

    broken["components"]["schemas"]["SystemStatus"]["examples"] = [
        {
            "device_id": "d",
            "model": "m",
            "firmware_version": "1",
            "frontend_version": "1",
            "boot_id": "b",
            "uptime_ms": "1",
            "wall_time": "yesterday afternoon",
            "active_job_ids": [],
        }
    ]
    findings = _checked(broken, "examples_valid")
    assert any("wall_time" in f.pointer for f in findings)


def test_findings_name_a_pointer_a_reader_can_follow(document: Document) -> None:
    """A 4700-line document needs a location, not a verdict."""
    raw = copy.deepcopy(document.raw)
    raw["components"]["schemas"]["Job"]["examples"][0]["progress"]["unit"] = "furlongs"
    finding = _checked(raw, "examples_valid")[0]
    assert finding.pointer == "#/components/schemas/Job/examples/0/progress/unit"
    assert str(finding).startswith("examples_valid: #/components/schemas/Job")


# -- the command-line entry point -----------------------------------------


def test_the_cli_exits_zero_on_the_real_document(capsys: Any) -> None:
    from cedar_contract.checks.__main__ import main

    assert main([]) == 0
    printed = capsys.readouterr().out
    assert "ok   refs_resolve" in printed
    assert "skip undocumented_routes" in printed


def test_the_cli_exits_nonzero_and_says_where(
    tmp_path: Any, document: Document, capsys: Any
) -> None:
    from cedar_contract.checks.__main__ import main

    raw = copy.deepcopy(document.raw)
    raw["paths"]["/matter/status"]["get"]["operationId"] = "getSystemStatus"
    path = tmp_path / "openapi.json"
    path.write_text(json.dumps(raw))
    assert main([str(path)]) == 1
    assert "FAIL operation_ids" in capsys.readouterr().out
