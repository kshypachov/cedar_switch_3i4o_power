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
    # 37 since the STM32 update stage: getSystemFirmware and startSystemUpdate,
    # with SystemFirmware, SystemUpdateSummary and SystemUpdateRequest.
    # 40 since the coredump (2026-09-18): getCoredump, downloadCoredump and
    # clearCoredump, with Coredump and CoredumpStatus.
    assert len(document.operations) == 40
    assert len(document.raw["paths"]) == 33
    assert len(document.schemas) == 53


def test_all_five_checks_run() -> None:
    """Section 12 lists five. The fifth was deferred in P1 for want of a route
    table; P2 gave the device one, so nothing is deferred any more."""
    assert checks.DEFERRED == {}
    assert list(checks.CHECKS) == [
        "refs_resolve",
        "path_parameters",
        "operation_ids",
        "examples_valid",
        "undocumented_routes",
    ]


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
    assert "ok   undocumented_routes" in printed
    assert "info the device serves" in printed


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


# -- the fifth check on damaged route tables -------------------------------


def _route_findings(document: Document, tmp_path: Any, edit: Any, edit_resources: Any = None) -> list[str]:
    routes = checks.ROUTES_FILE.read_text()
    resources = checks.RESOURCES_FILE.read_text()
    routes_path = tmp_path / "routes.h"
    resources_path = tmp_path / "http_resources.h"
    routes_path.write_text(edit(routes))
    resources_path.write_text(edit_resources(resources) if edit_resources else resources)
    return [f.message for f in checks.check_undocumented_routes(document, routes_path, resources_path)]


def _replace_once(old: str, new: str) -> Any:
    def edit(text: str) -> str:
        assert text.count(old) == 1, old
        return text.replace(old, new)
    return edit


def _upload_chunk_route(flags: str) -> str:
    return (f'WEB_API_V1_ROUTE(writeUploadChunk, PUT, "/firmware/uploads/{{upload_id}}/data", {flags}, '
            "V1_NO_BODY, NULL, v1_upload_chunk_query, v1_write_upload_chunk)\n")


def test_a_raw_body_route_matches_an_octet_stream_body(document: Document, tmp_path: Any) -> None:
    raw = "WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED | WEB_API_BODY_OCTETS"
    messages = _route_findings(document, tmp_path, lambda t: t + _upload_chunk_route(raw))
    assert not any(m.startswith("writeUploadChunk:") for m in messages), messages

    without = "WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED"
    messages = _route_findings(document, tmp_path, lambda t: t + _upload_chunk_route(without))
    assert "writeUploadChunk: declares no body schema, but the document has a request body" in messages


def test_a_body_of_the_wrong_media_type_is_found(document: Document, tmp_path: Any) -> None:
    messages = _route_findings(
        document, tmp_path,
        _replace_once("WEB_API_PUBLIC | WEB_API_ORIGIN | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_login_body)",
                      "WEB_API_PUBLIC | WEB_API_ORIGIN | WEB_API_BODY_REQUIRED | WEB_API_BODY_OCTETS, "
                      "V1_BODY(struct v1_login_body)"),
    )
    assert ("login: routed as a raw application/octet-stream body, "
            "the document's request body is application/json") in messages, messages
    json_chunk = ('WEB_API_V1_ROUTE(writeUploadChunk, PUT, "/firmware/uploads/{upload_id}/data", '
                  "WEB_API_CSRF | WEB_API_IDEMPOTENT | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_empty_body), "
                  "&v1_empty_schema, v1_upload_chunk_query, v1_write_upload_chunk)\n")
    messages = _route_findings(document, tmp_path, lambda t: t + json_chunk)
    assert ("writeUploadChunk: routed as a JSON body, "
            "the document's request body is application/octet-stream") in messages, messages


def test_the_route_table_is_read(document: Document) -> None:
    routes, problems = checks.parse_routes(checks.ROUTES_FILE.read_text())
    assert problems == []
    assert {"getAuthState", "setupAdmin", "login", "logout", "changePassword"} <= {
        r.operation_id for r in routes
    }


def test_an_undeclared_operation_is_found(document: Document, tmp_path: Any) -> None:
    extra = 'WEB_API_V1_ROUTE(rebootDevice, POST, "/system/reboot", 0, V1_NO_BODY, NULL, NULL, h)\n'
    messages = _route_findings(document, tmp_path, lambda t: t + extra,
                               lambda r: r + 'WEB_API_V1_RESOURCE(x, "/api/v1/system/reboot")\n')
    assert any("rebootDevice" in m and "does not declare" in m for m in messages), messages


def test_a_wrong_method_or_path_is_found(document: Document, tmp_path: Any) -> None:
    messages = _route_findings(
        document, tmp_path, _replace_once("WEB_API_V1_ROUTE(logout, DELETE,", "WEB_API_V1_ROUTE(logout, POST,")
    )
    assert any("logout is DELETE /auth/session in the document" in m for m in messages), messages
    messages = _route_findings(
        document, tmp_path, _replace_once('getCapabilities, GET, "/capabilities"', 'getCapabilities, GET, "/caps"'),
        lambda r: r.replace('"/api/v1/capabilities"', '"/api/v1/caps"'),
    )
    assert any("routed as GET /caps" in m for m in messages), messages


def test_a_missing_csrf_requirement_is_found(document: Document, tmp_path: Any) -> None:
    messages = _route_findings(
        document, tmp_path, _replace_once("WEB_API_V1_ROUTE(logout, DELETE, \"/auth/session\", WEB_API_CSRF,",
                                          "WEB_API_V1_ROUTE(logout, DELETE, \"/auth/session\", 0,")
    )
    assert any("logout: WEB_API_CSRF missing" in m for m in messages), messages


def test_a_public_route_the_document_protects_is_found(document: Document, tmp_path: Any) -> None:
    messages = _route_findings(
        document, tmp_path, _replace_once('getSystemStatus, GET, "/system/status", 0,',
                                          'getSystemStatus, GET, "/system/status", WEB_API_PUBLIC,')
    )
    assert any("getSystemStatus: routed public" in m for m in messages), messages


def test_body_mismatches_are_found(document: Document, tmp_path: Any) -> None:
    messages = _route_findings(
        document, tmp_path,
        _replace_once("WEB_API_PUBLIC | WEB_API_ORIGIN | WEB_API_BODY_REQUIRED, V1_BODY(struct v1_login_body), &v1_login_schema,",
                      "WEB_API_PUBLIC | WEB_API_ORIGIN, V1_BODY(struct v1_login_body), &v1_login_schema,"),
    )
    assert any("login: WEB_API_BODY_REQUIRED disagrees" in m for m in messages), messages
    messages = _route_findings(
        document, tmp_path,
        _replace_once('getJob, GET, "/jobs/{job_id}", 0, V1_NO_BODY, NULL,', 'getJob, GET, "/jobs/{job_id}", 0, V1_NO_BODY, &x,'),
    )
    assert any("getJob: decodes a body" in m for m in messages), messages


def test_unreadable_duplicate_and_resource_problems_are_found(document: Document, tmp_path: Any) -> None:
    messages = _route_findings(document, tmp_path, lambda t: t + "WEB_API_V1_ROUTE(broken\n")
    assert "a route line this check cannot read" in messages
    line = next(l for l in checks.ROUTES_FILE.read_text().splitlines() if l.startswith("WEB_API_V1_ROUTE(getJob,"))
    messages = _route_findings(document, tmp_path, lambda t: t + line + "\n")
    assert "getJob is routed twice" in messages
    messages = _route_findings(document, tmp_path, lambda t: t,
                               lambda r: r.replace('WEB_API_V1_RESOURCE(web_api_06_jobs, "/api/v1/jobs/*")\n', ""))
    assert "no server resource for /api/v1/jobs/*" in messages
    messages = _route_findings(document, tmp_path, lambda t: t,
                               lambda r: r + 'WEB_API_V1_RESOURCE(web_api_99, "/api/v1/unrouted")\n')
    assert "resource /api/v1/unrouted has no route" in messages
