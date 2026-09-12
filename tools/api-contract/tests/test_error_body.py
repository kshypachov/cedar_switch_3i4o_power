# SPDX-License-Identifier: Apache-2.0
"""The claim that the mock's rejection is the device's, held to account.

`errors.py` says it is a transcription of `api_validation.c`. A transcription is
worth nothing unless something notices when the original changes, so these tests
parse the C source and compare. They are the reason the README can say "byte for
byte" without it being a hope.

What is checked against the C source: every row of the error table (name, status,
retryable), the closed set of field codes, the truncation note, and the three
bounds. What is checked against the contract instead: that the serialised body
reproduces the example in `api-contract.md` exactly, and that it validates
against the `Error` schema.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest
from cedar_contract import errors
from cedar_contract.openapi import Document

REPO = Path(__file__).resolve().parents[3]
C_SOURCE = REPO / "modules" / "api-validation" / "lib" / "api_validation.c"
C_HEADER = REPO / "modules" / "api-validation" / "include" / "api_validation" / "api_validation.h"
C_KCONFIG = REPO / "modules" / "api-validation" / "lib" / "Kconfig"


@pytest.fixture(scope="module")
def c_source() -> str:
    assert C_SOURCE.exists(), f"the module under transcription is missing: {C_SOURCE}"
    return C_SOURCE.read_text(encoding="utf-8")


def _c_error_table(source: str) -> list[tuple[str, int, bool]]:
    """The rows of `error_table`, in declaration order."""
    body = source[source.index("static const struct error_entry error_table") :]
    body = body[: body.index("\n};")]
    rows = re.findall(
        r"\[API_ERR_[A-Z_]+\]\s*=\s*\{\s*\"([a-z_]+)\"\s*,\s*(\d+)\s*,\s*(true|false)\s*\}",
        body,
    )
    return [(name, int(status), flag == "true") for name, status, flag in rows]


def test_error_table_matches_the_c_module(c_source: str) -> None:
    """Row for row, in order. Order matters because both are transcriptions of
    the contract's table and reading them side by side is how a reviewer checks
    the third one."""
    from_c = _c_error_table(c_source)
    from_python = [(name, status, retry) for name, (status, retry) in errors.ERROR_TABLE.items()]
    assert from_c, "no rows found in the C table; the parser needs updating"
    assert from_python == from_c


def test_every_contract_code_is_present(c_source: str) -> None:
    """The enum and the table agree in C, so a code in the enum with no row is a
    hole this would fall into silently."""
    header = C_HEADER.read_text(encoding="utf-8")
    enum = re.findall(r"^\tAPI_ERR_([A-Z_0-9]+)(?:\s*=\s*\d+)?,?$", header, re.MULTILINE)
    names = {name.lower() for name in enum if name != "COUNT"}
    assert names == set(errors.ERROR_TABLE)


def test_field_codes_match_the_c_module(c_source: str) -> None:
    body = c_source[c_source.index("field_code_names[API_FIELD_CODE_COUNT]") :]
    body = body[: body.index("\n};")]
    found = re.findall(r"\[API_FIELD_[A-Z_]+\]\s*=\s*\"([a-z_]+)\"", body)
    assert tuple(found) == errors.FIELD_CODES


def test_bounds_match_the_c_module() -> None:
    header = C_HEADER.read_text(encoding="utf-8")
    assert f"#define API_ERROR_MESSAGE_MAX_LEN {errors.MESSAGE_MAX_BYTES}" in header
    assert f"#define API_ERROR_PATH_MAX_LEN {errors.PATH_MAX_BYTES}" in header
    kconfig = C_KCONFIG.read_text(encoding="utf-8")
    assert f"default {errors.MAX_FIELDS}" in kconfig


def test_truncation_note_matches_the_c_module(c_source: str) -> None:
    """The note lives in the message because `ErrorDetail` is
    `additionalProperties: false`. If the C text changes, a client keying off it
    breaks, so it is pinned."""
    assert f'"{errors.TRUNCATION_NOTE}"' in c_source


# -- the serialiser --------------------------------------------------------


def test_serialised_body_reproduces_the_contract_example() -> None:
    err = errors.error(
        "validation_failed",
        "Static IPv4 requires an address and prefix",
        [("/interfaces/ethernet/ipv4/address", "required")],
    )
    err.request_id = "req_7b22"
    assert err.to_json() == (
        b'{"error":{"code":"validation_failed",'
        b'"message":"Static IPv4 requires an address and prefix",'
        b'"request_id":"req_7b22","retryable":false,'
        b'"fields":[{"path":"/interfaces/ethernet/ipv4/address","code":"required"}]}}'
    )


def test_fields_is_omitted_and_not_empty() -> None:
    """An empty array reads as "we checked the fields and they are fine", which
    is not what a rejection with no field detail means."""
    body = errors.error("busy", "later").to_json()
    assert b"fields" not in body


def test_escaping_follows_the_c_source_not_python() -> None:
    """The two disagree, and the device's answer is the one that matters.

    `json.dumps` writes `\\b` and `\\f` for backspace and form feed and escapes
    non-ASCII; the device writes `\\u0008`, `\\u000c` and raw UTF-8.
    """
    err = errors.error("internal_error", 'q"\\ t\t n\n b\x08 f\x0c ü')
    body = err.to_json().decode("utf-8")
    assert '\\"' in body and "\\\\" in body
    assert "\\t" in body and "\\n" in body
    assert "\\u0008" in body and "\\u000c" in body
    assert "ü" in body, "UTF-8 passes through, as the contract says bodies are UTF-8"
    assert json.loads(body)["error"]["message"].endswith("ü")


def test_message_is_cut_to_the_device_buffer() -> None:
    err = errors.error("internal_error", "y" * 400)
    message = json.loads(err.to_json())["error"]["message"]
    assert len(message) == errors.MESSAGE_MAX_BYTES


def test_overflowing_fields_is_recorded_in_the_message() -> None:
    """A client shown six of nine bad fields fixes six and is rejected again
    with no hint that more were waiting."""
    err = errors.ApiError(code="validation_failed", message="too many")
    for index in range(errors.MAX_FIELDS + 3):
        err.add_field(f"/field{index}", "required")
    detail = json.loads(err.to_json())["error"]
    assert len(detail["fields"]) == errors.MAX_FIELDS
    assert detail["message"].endswith(errors.TRUNCATION_NOTE)


def test_status_and_retryable_cannot_be_chosen_at_the_call_site() -> None:
    """They are properties of the code, which is the module's whole reason for
    existing. The only way to change either is to change the table."""
    assert errors.error("stale_revision").status == 409
    assert errors.error("busy").retryable is True
    assert errors.error("stale_revision").retryable is False
    assert errors.error("capability_unavailable").retryable is False
    assert errors.error("service_not_ready").retryable is True


def test_an_unknown_code_is_refused_rather_than_guessed() -> None:
    with pytest.raises(ValueError):
        errors.error("teapot")


def test_a_field_code_outside_the_closed_set_is_refused() -> None:
    with pytest.raises(ValueError):
        errors.ErrorField("/x", "too_short")


def test_a_field_path_must_be_a_json_pointer() -> None:
    with pytest.raises(ValueError):
        errors.ErrorField("interfaces/ethernet", "required")


def test_request_ids_match_the_document_pattern(document: Document) -> None:
    ids = errors.RequestIds(seed=0xFFFFFFFE)
    produced = [ids.next() for _ in range(4)]
    assert produced[:3] == ["req_ffffffff", "req_00000000", "req_00000001"]
    validator = document.validator({"$ref": "#/components/schemas/ErrorDetail"})
    for request_id in produced:
        detail = {"code": "busy", "message": "", "request_id": request_id, "retryable": True}
        assert not list(validator.iter_errors(detail))


def test_every_serialised_body_validates_against_the_schema(document: Document) -> None:
    """All thirty of them, because `Error` is the one schema every operation can
    answer with and a body that fails it fails everywhere at once."""
    validator = document.validator({"$ref": "#/components/schemas/Error"})
    for code in errors.ERROR_TABLE:
        err = errors.error(code, f"a message about {code}", [("/x", "invalid_format")])
        payload = json.loads(err.to_json())
        assert not list(validator.iter_errors(payload)), code
        assert payload["error"]["code"] == code
        assert payload["error"]["retryable"] is errors.is_retryable(code)


def test_detail_dict_agrees_with_the_serialiser() -> None:
    """A job carries the same `ErrorDetail` as a rejection, so the two paths must
    not drift: same keys, same order, same bounded message."""
    err = errors.ApiError(code="validation_failed", message="m" * 200)
    for index in range(errors.MAX_FIELDS + 1):
        err.add_field(f"/f{index}", "required")
    assert errors.detail_dict(err) == json.loads(err.to_json())["error"]
    assert list(errors.detail_dict(err)) == [
        "code",
        "message",
        "request_id",
        "retryable",
        "fields",
    ]
