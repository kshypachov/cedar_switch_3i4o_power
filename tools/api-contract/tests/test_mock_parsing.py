# SPDX-License-Identifier: Apache-2.0
"""The body rules the device's decoder enforces, reproduced.

modules/web-api/lib/json_reader.c decides every edge of JSON explicitly, and
tests/web_api/src/json_reader.c tests it. Python's json module answers some of
those edges differently, so the mock closes each gap; these tests pin the same
answers on this side. A frontend must not be able to tell the device from the
mock by the way either one refuses a body.
"""

from __future__ import annotations

import pytest

from cedar_contract.errors import ApiError
from cedar_contract.mock.app import _validate_request, host_allowed, parse_json_strictly
from cedar_contract.mock.wire import Request

from .conftest import BASE, PASSWORD, Harness

# -- Host ----------------------------------------------------------------------


@pytest.mark.parametrize(
    "host",
    ["192.168.88.14", "192.168.88.14:80", "[fe80::8234:28ff:fe10:1273]", "[fe80::1]:8080",
     "[::1]", "localhost", "LocalHost:3000"],
)
def test_host_literals_and_localhost_are_allowed(host: str) -> None:
    assert host_allowed(host)


@pytest.mark.parametrize(
    "host",
    ["", "evil.example", "192.168.88.14:99999", "192.168.88.14:", "[fe80::1", "fe80::1",
     "010.0.0.1", "192.168.88", "[fe80::1]x", "localhost.", "[fe80::1%en0]", "a" * 64],
)
def test_other_hosts_are_refused(host: str) -> None:
    assert not host_allowed(host)


def test_extra_hosts() -> None:
    assert host_allowed("cedar.lan:80", "cedar.lan, Switch.local")
    assert host_allowed("switch.LOCAL", "cedar.lan, Switch.local")
    assert not host_allowed("cedar.lan.evil", "cedar.lan")
    assert host_allowed(None), "no Host is not a browser"


def test_api_requests_with_a_foreign_host_are_refused_first(harness: Harness) -> None:
    """Before routing: an unknown path and a known one get the same answer."""
    for path in ("/auth/state", "/nowhere"):
        response = harness.client.get(path, headers={"host": "rebound.example"})
        assert response.status == 403
        assert response.json["error"]["code"] == "origin_rejected"


def test_the_setup_token_cannot_be_read_through_a_rebound_name(fresh: Harness) -> None:
    response = fresh.client.get("/auth/state", headers={"host": "attacker.example:8080"},
                                authenticate=False)
    assert response.status == 403
    assert "setup_token" not in response.body.decode()


def test_extra_hosts_scenario(harness: Harness) -> None:
    harness.state.scenario.extra_hosts = "cedar.lan"
    assert harness.client.get("/system/status", headers={"host": "cedar.lan"}).status == 200


# -- syntax the device refuses -------------------------------------------------


@pytest.mark.parametrize(
    "raw",
    [
        b'{"password":"a","password":"b"}',
        b'{"password":"a","x":{"k":1,"k":2}}',
        b'{"password":"a","p\\u0061ssword":"b"}',
        b'{"password":NaN}',
        b'{"password":Infinity}',
        b'{"password":"\xc0\xaf"}',
        b'{"password":"\xed\xa0\x80"}',
        b'{"password":"\xff"}',
        b'\xff\xfe{\x00}\x00',
        b'{"password":"a"} x',
        b'{"x":' + b"[" * 8 + b"]" * 8 + b"}",
        b"{" + b",".join(b'"k%d":1' % i for i in range(65)) + b"}",
    ],
)
def test_syntax_refusals(raw: bytes) -> None:
    with pytest.raises(ApiError) as caught:
        parse_json_strictly(raw)
    assert caught.value.code == "invalid_json"


def test_nesting_at_the_limit_is_well_formed() -> None:
    parse_json_strictly(b'{"x":' + b"[" * 7 + b"]" * 7 + b"}")
    parse_json_strictly(b"{" + b",".join(b'"k%d":1' % i for i in range(64)) + b"}")


def test_a_duplicate_name_reaches_the_client_as_invalid_json(fresh: Harness) -> None:
    response = fresh.app.handle(
        Request("POST", f"{BASE}/auth/session", {"content-type": "application/json"},
                b'{"password":"x","password":"y"}')
    )
    assert response.status == 400
    assert response.json["error"]["code"] == "invalid_json"


@pytest.mark.parametrize(
    ("text", "value"),
    [("120", 120), ("120.0", 120), ("1.2e2", 120), ("12E1", 120), ("1200e-1", 120),
     ("-0", 0), ("0.0e5", 0), ("100e-2", 1)],
)
def test_integers_are_values(text: str, value: int) -> None:
    assert parse_json_strictly(('{"n":%s}' % text).encode())["n"] == value


def test_fractions_stay_fractions_and_huge_values_are_clamped() -> None:
    assert isinstance(parse_json_strictly(b'{"n":1.5}')["n"], float)
    huge = parse_json_strictly(b'{"n":1e99999999}')["n"]
    assert isinstance(huge, int) and huge > 2**63, "out of range, without building the number"


# -- the field list ------------------------------------------------------------

OBJECT = {
    "type": "object",
    "properties": {
        "name": {"type": "string", "minLength": 2, "maxLength": 16},
        "note": {"anyOf": [{"type": "string", "maxLength": 8}, {"type": "null"}]},
        "count": {"type": "integer", "minimum": 0, "maximum": 1000},
        "mode": {"type": "string", "enum": ["dhcp", "static"], "maxLength": 7},
        "items": {"type": "array", "items": {"type": "string"}, "minItems": 1, "maxItems": 3},
        "inner": {
            "type": "object",
            "properties": {"level": {"type": "integer"}, "on": {"type": "boolean"}},
            "required": ["level"],
            "additionalProperties": False,
        },
    },
    "required": ["name"],
    "additionalProperties": False,
}


def fields(document, body) -> list[tuple[str, str]]:
    with pytest.raises(ApiError) as caught:
        _validate_request(document, OBJECT, body)
    assert caught.value.code == "validation_failed"
    return [(f.path, f.code) for f in caught.value.fields]


def test_every_unknown_member_is_reported_sorted(document) -> None:
    assert fields(document, {"zeta": 1, "name": "ab", "alpha": {"a": [1]}}) == [
        ("/alpha", "unknown_field"), ("/zeta", "unknown_field"),
    ]


def test_entries_sort_by_segment(document) -> None:
    body = {"name": "ab", "inner": {"on": 1, "level": "x"}, "count": "x", "aa": 1, "inn": 1}
    assert fields(document, body) == [
        ("/aa", "unknown_field"), ("/count", "invalid_format"), ("/inn", "unknown_field"),
        ("/inner/level", "invalid_format"), ("/inner/on", "invalid_format"),
    ]


def test_array_indices_sort_as_numbers(document) -> None:
    schema = {"type": "object", "properties": {"v": {"type": "array", "items": {"type": "boolean"}}}}
    body = {"v": [True, True, 1, True, True, True, True, True, True, True, 1]}
    with pytest.raises(ApiError) as caught:
        _validate_request(document, schema, body)
    assert [f.path for f in caught.value.fields] == ["/v/2", "/v/10"]


def test_required_is_named_by_its_own_pointer(document) -> None:
    assert fields(document, {}) == [("/name", "required")]
    assert fields(document, {"name": "ab", "inner": {"x": True}}) == [
        ("/inner/level", "required"), ("/inner/x", "unknown_field"),
    ]


def test_a_nullable_value_reports_its_real_problem(document) -> None:
    """Not `invalid_format` from anyOf: the device reports what is wrong with the
    string, because to it the field is a nullable string."""
    assert fields(document, {"name": "ab", "note": "much too long"}) == [("/note", "too_long")]
    assert fields(document, {"name": "ab", "note": 5}) == [("/note", "invalid_format")]
    _validate_request(document, OBJECT, {"name": "ab", "note": None})


def test_one_entry_per_value_by_precedence(document) -> None:
    """Too long and not in the enum: too_long ranks first."""
    assert fields(document, {"name": "ab", "mode": "automatic"}) == [("/mode", "too_long")]
    assert fields(document, {"name": "ab", "mode": "auto"}) == [("/mode", "not_allowed")]


def test_text_c_cannot_hold(document) -> None:
    assert fields(document, {"name": "a\x00b"}) == [("/name", "invalid_format")]
    assert fields(document, {"name": "ab\ud800"}) == [("/name", "invalid_format")]
    # Inside a value that is already reported, the value's report stands.
    assert fields(document, {"name": "ab", "extra": "\x00"}) == [("/extra", "unknown_field")]
    # Two code points, so long enough; what is wrong is the NUL.
    assert fields(document, {"name": "x\x00"}) == [("/name", "invalid_format")]
    # One code point: too short ranks before the NUL.
    assert fields(document, {"name": "\x00"}) == [("/name", "out_of_range")]


def test_pointers_are_escaped_and_clipped(document) -> None:
    assert fields(document, {"name": "ab", "a/b~c": 1}) == [("/a~1b~0c", "unknown_field")]
    assert fields(document, {"name": "ab", "inner": {"level": 1, "b" * 70: 1}}) == [
        ("/inner", "unknown_field"),
    ]
    assert fields(document, {"name": "ab", "x\x01": 1, "y\x02": 1}) == [("/", "unknown_field")]


def test_arrays(document) -> None:
    assert fields(document, {"name": "ab", "items": []}) == [("/items", "out_of_range")]
    assert fields(document, {"name": "ab", "items": ["a", "b", "c", "d"]}) == [
        ("/items", "out_of_range"),
    ]


def test_truncation_keeps_the_first_by_path(document) -> None:
    schema = {"type": "object", "properties": {}, "additionalProperties": False}
    with pytest.raises(ApiError) as caught:
        _validate_request(document, schema, {k: 1 for k in "gfedcba"})
    assert [f.path for f in caught.value.fields] == ["/a", "/b", "/c", "/d", "/e", "/f"]
    assert caught.value.fields_truncated


def test_login_still_works_after_all_this(harness: Harness) -> None:
    assert harness.client.login(PASSWORD).status == 200
