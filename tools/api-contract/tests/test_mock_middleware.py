# SPDX-License-Identifier: Apache-2.0
"""The checks every mutation goes through, and the order they go through them in.

The order is a contract requirement, not a preference: authentication before
CSRF, because there is no token to check without a session; CSRF before the body,
because a rejected request must not be parsed; the size limit before the parse,
because the limit exists so a large body is never buffered; and the idempotency
key before the handler, because a replay must return the first answer without
running the action a second time.

Also here: that the router comes from the document. A required header the document
declares is enforced without anything in the mock naming it, which is the property
that keeps the two from drifting.
"""

from __future__ import annotations

from cedar_contract.mock.constants import JSON_BODY_BYTES
from cedar_contract.mock.wire import Request
from cedar_contract.openapi import Document

from .conftest import BASE, PASSWORD, Harness, build

CANDIDATE_PATH = "/network/transactions"


# -- authentication and CSRF ----------------------------------------------


def test_the_public_operations_are_exactly_the_ones_the_document_marks_public(
    document: Document,
) -> None:
    """`security: []` in the document is the whole rule, and the mock reads it
    rather than keeping its own list — which is why this can be an equality."""
    public = {op.operation_id for op in document.operations if not op.requires_session}
    assert public == {"getAuthState", "setupAdmin", "login"}


def test_every_protected_operation_refuses_an_anonymous_caller(document: Document) -> None:
    """All thirty-two of them, so a route cannot be forgotten. The contract says
    codes, logs, capabilities and uploads are protected too — the tempting
    exceptions."""
    harness = build(document, setup_required=False)
    for operation in document.operations:
        if not operation.requires_session:
            continue
        path = operation.path
        for name in operation.template_parameters:
            path = path.replace(f"{{{name}}}", "someid")
        response = harness.app.handle(Request(operation.method, f"{BASE}{path}"))
        assert response.status == 401, f"{operation.operation_id} answered {response.status}"
        assert response.json["error"]["code"] == "authentication_required"


def test_a_mutation_without_a_csrf_token_is_refused(harness: Harness) -> None:
    response = harness.client.request(
        "POST",
        "/network/wifi/scans",
        {},
        headers={"x-csrf-token": ""},
    )
    assert response.status == 403
    assert response.json["error"]["code"] == "csrf_failed"


def test_a_csrf_token_from_another_session_is_refused(harness: Harness) -> None:
    other = harness.client.__class__(app=harness.app, document=harness.client.document)
    other.login()
    response = harness.client.request(
        "POST", "/network/wifi/scans", {}, headers={"x-csrf-token": other.csrf or ""}
    )
    assert response.status == 403
    assert response.json["error"]["code"] == "csrf_failed"


def test_authentication_is_checked_before_the_csrf_token(harness: Harness) -> None:
    """Otherwise the answer to an anonymous request would be `csrf_failed`, which
    tells the client to fetch a token it cannot have.

    Both are wrong in the second request, which is what pins the order: with the
    checks the other way round, a caller with no session and a bogus token would
    be told about the token.
    """
    response = harness.app.handle(
        Request("POST", f"{BASE}/network/wifi/scans", {"content-type": "application/json"}, b"{}")
    )
    assert response.json["error"]["code"] == "authentication_required"

    both_wrong = harness.app.handle(
        Request(
            "POST",
            f"{BASE}/network/wifi/scans",
            {
                "content-type": "application/json",
                "x-csrf-token": "not-a-token-at-all-but-long-enough",
                "idempotency-key": "order-key-0000000001",
            },
            b"{}",
        )
    )
    assert both_wrong.json["error"]["code"] == "authentication_required"


def test_the_csrf_token_is_checked_before_the_body(harness: Harness) -> None:
    """A rejected request must not be parsed, and a client that sent both a bad
    token and a bad body needs to be told about the token first."""
    response = harness.client.request(
        "POST",
        CANDIDATE_PATH,
        {"nonsense": True},
        headers={"x-csrf-token": "wrong"},
    )
    assert response.json["error"]["code"] == "csrf_failed"


def test_a_get_needs_no_csrf_token(harness: Harness) -> None:
    """The contract asks for one on mutations only, and the document says which
    operations declare the header."""
    response = harness.client.request(
        "GET", "/system/status", headers={"x-csrf-token": "not-a-token"}
    )
    assert response.status == 200


def test_login_checks_the_origin_instead_of_a_token(harness: Harness) -> None:
    """It runs before a token exists, so `Origin` is what there is."""
    response = harness.client.post(
        "/auth/session",
        {"password": PASSWORD},
        headers={"origin": "http://evil.example", "host": "192.168.88.14"},
        authenticate=False,
    )
    assert response.status == 403
    assert response.json["error"]["code"] == "origin_rejected"


def test_a_matching_origin_is_accepted(harness: Harness) -> None:
    response = harness.client.post(
        "/auth/session",
        {"password": PASSWORD},
        headers={"origin": "http://192.168.88.14", "host": "192.168.88.14"},
        authenticate=False,
    )
    assert response.status == 200


def test_an_absent_origin_is_allowed(harness: Harness) -> None:
    """A browser always sends one on a cross-site POST, which is the case being
    defended against; refusing its absence would break `curl` without protecting
    anything."""
    assert harness.client.login().status == 200


# -- required headers -----------------------------------------------------


def test_a_missing_idempotency_key_is_refused(harness: Harness) -> None:
    """DECISION, recorded in `app.py` and the README: `validation_failed` with no
    `fields` entry. `fields` holds JSON Pointers into the body, and inventing a
    pointer syntax for headers would put a shape in the error body that the
    document does not describe — so the header is named in the message."""
    response = harness.client.request(
        "POST", "/network/wifi/scans", {}, headers={"idempotency-key": ""}
    )
    assert response.status == 422
    detail = response.json["error"]
    assert detail["code"] == "validation_failed"
    assert "Idempotency-Key" in detail["message"]
    assert "fields" not in detail


def test_an_absent_idempotency_key_is_refused(harness: Harness) -> None:
    """Absent, not empty. The two take different paths through the middleware —
    one fails the declared schema, the other fails the presence check — and a
    request with no key at all must not simply proceed without deduplication.
    """
    assert harness.client.csrf is not None
    response = harness.app.handle(
        Request(
            "POST",
            f"{BASE}/network/wifi/scans",
            {
                "cookie": f"cedar_session={harness.client.cookie}",
                "x-csrf-token": harness.client.csrf,
                "content-type": "application/json",
            },
            b"{}",
        )
    )
    assert response.status == 422
    assert "Idempotency-Key" in response.json["error"]["message"]
    assert len(harness.state.jobs) == 0, "and the action did not run"


def test_a_malformed_idempotency_key_is_refused_against_the_declared_schema(
    harness: Harness,
) -> None:
    """16-64 characters from a restricted set. Nothing in the mock repeats those
    bounds: they come from the document."""
    for key in ("short", "k" * 65, "has spaces in it!!!!"):
        response = harness.client.post("/network/wifi/scans", {}, key=key)
        assert response.status == 422, key
        assert "Idempotency-Key" in response.json["error"]["message"]


def test_a_key_at_the_declared_bounds_is_accepted(harness: Harness) -> None:
    assert harness.client.post("/network/wifi/scans", {}, key="k" * 16).status == 202


# -- idempotency ----------------------------------------------------------


def test_the_same_key_and_body_returns_the_same_answer(harness: Harness) -> None:
    """And does not run the action twice, which is the whole point: a retried
    scan must not start a second radio sweep."""
    key = "repeat-key-000000001"
    first = harness.client.post("/network/wifi/scans", {}, key=key)
    second = harness.client.post("/network/wifi/scans", {}, key=key)
    assert first.status == second.status == 202
    assert first.json == second.json
    assert len(harness.state.jobs) == 1


def test_the_same_key_with_a_different_body_is_a_conflict(harness: Harness) -> None:
    key = "conflict-key-00000001"
    from .conftest import VALID_CANDIDATE, candidate

    revision = harness.client.get("/network/config").json["revision"]
    first = harness.client.post(
        CANDIDATE_PATH, {"base_revision": revision, "config": VALID_CANDIDATE}, key=key
    )
    assert first.status == 201
    second = harness.client.post(
        CANDIDATE_PATH,
        {
            "base_revision": revision,
            "config": candidate(**{"interfaces/ethernet/ipv4/address": "192.168.88.51"}),
        },
        key=key,
    )
    assert second.status == 409
    assert second.json["error"]["code"] == "idempotency_conflict"


def test_a_rejection_is_not_replayed(harness: Harness) -> None:
    """The client is expected to fix the request and send it again under the same
    key, so storing the refusal would lock it out of ever succeeding."""
    key = "rejected-key-00000001"
    revision = harness.client.get("/network/config").json["revision"]
    first = harness.client.post(
        CANDIDATE_PATH, {"base_revision": revision + 7, "config": {}}, key=key
    )
    assert first.status == 422
    from .conftest import VALID_CANDIDATE

    second = harness.client.post(
        CANDIDATE_PATH, {"base_revision": revision, "config": VALID_CANDIDATE}, key=key
    )
    assert second.status == 201


def test_a_rejection_does_not_disturb_another_keys_record(harness: Harness) -> None:
    """The dedupe table is not scratch space: an unrelated refusal in between must
    not cost a client the replay it is entitled to."""
    key = "kept-key-0000000001"
    first = harness.client.post("/network/wifi/scans", {}, key=key)
    assert harness.client.post(CANDIDATE_PATH, {"base_revision": 0}).status == 422
    assert harness.client.post("/network/wifi/scans", {}, key=key).json == first.json
    assert len(harness.state.jobs) == 1


def test_the_scope_includes_the_url_not_only_the_key(harness: Harness) -> None:
    """Two different operations under one key are two different actions."""
    key = "scoped-key-000000001"
    assert harness.client.post("/network/wifi/scans", {}, key=key).status == 202
    assert harness.client.delete("/matter/commissioning", key=key).status == 202


def test_the_scope_survives_a_new_session(harness: Harness) -> None:
    """The contract scopes to the admin principal, not to the cookie, so a replay
    after a re-login still deduplicates — which is the case the rule exists for."""
    key = "principal-key-0000001"
    first = harness.client.post("/network/wifi/scans", {}, key=key)
    harness.client.delete("/auth/session")
    harness.client.login()
    second = harness.client.post("/network/wifi/scans", {}, key=key)
    assert second.json == first.json
    assert len(harness.state.jobs) == 1


def test_login_and_logout_need_no_key(harness: Harness) -> None:
    """The contract exempts them, and requiring one would make signing in depend
    on the client generating a key before it has a session."""
    response = harness.app.handle(
        Request(
            "POST",
            f"{BASE}/auth/session",
            {"content-type": "application/json"},
            b'{"password": "cedar-mock-admin"}',
        )
    )
    assert response.status == 200


# -- bodies ---------------------------------------------------------------


def test_a_malformed_json_body_is_invalid_json(harness: Harness) -> None:
    response = harness.client.request(
        "POST",
        CANDIDATE_PATH,
        raw=b"{not json",
        headers={"content-type": "application/json"},
    )
    assert response.status == 400
    assert response.json["error"]["code"] == "invalid_json"


def test_an_oversized_body_is_refused_before_it_is_parsed(harness: Harness) -> None:
    """The published limit, enforced at the number `capabilities` reports."""
    limit = harness.client.get("/capabilities").json["limits"]["json_body_bytes"]
    assert limit == JSON_BODY_BYTES
    response = harness.client.request(
        "POST",
        CANDIDATE_PATH,
        raw=b'{"base_revision": 1, "pad": "' + b"x" * limit + b'"}',
        headers={"content-type": "application/json"},
    )
    assert response.status == 413
    assert response.json["error"]["code"] == "payload_too_large"


def test_a_wrong_content_type_is_unsupported_media_type(harness: Harness) -> None:
    response = harness.client.request(
        "POST",
        CANDIDATE_PATH,
        raw=b"base_revision=1",
        headers={"content-type": "application/x-www-form-urlencoded"},
    )
    assert response.status == 415
    assert response.json["error"]["code"] == "unsupported_media_type"


def test_a_missing_required_body_is_refused(harness: Harness) -> None:
    response = harness.client.request("POST", CANDIDATE_PATH)
    assert response.status == 400
    assert response.json["error"]["code"] == "invalid_json"


def test_an_unknown_field_is_rejected_with_its_pointer(harness: Harness) -> None:
    """The contract rejects unknown fields in requests rather than ignoring them,
    so that a typo in a client is found by the client's own author."""
    from .conftest import VALID_CANDIDATE

    revision = harness.client.get("/network/config").json["revision"]
    response = harness.client.post(
        CANDIDATE_PATH,
        {"base_revision": revision, "config": VALID_CANDIDATE, "apply_now": True},
    )
    assert response.status == 422
    assert response.json["error"]["fields"] == [{"path": "/apply_now", "code": "unknown_field"}]


def test_a_missing_required_field_names_the_field(harness: Harness) -> None:
    response = harness.client.post(CANDIDATE_PATH, {"base_revision": 1})
    assert response.json["error"]["fields"] == [{"path": "/config", "code": "required"}]


def test_a_wrong_type_is_an_invalid_format_field_error(harness: Harness) -> None:
    response = harness.client.post(CANDIDATE_PATH, {"base_revision": "one", "config": {}})
    codes = {f["path"]: f["code"] for f in response.json["error"]["fields"]}
    assert codes["/base_revision"] == "invalid_format"


def test_a_binary_operation_takes_binary(harness: Harness) -> None:
    upload = harness.client.post(
        "/firmware/uploads", {"filename": "c6.bin", "size_bytes": 64, "sha256": "0" * 64}
    ).json
    response = harness.client.request(
        "PUT",
        f"/firmware/uploads/{upload['id']}/data?offset=0",
        raw=b"\x00" * 64,
        headers={"content-type": "application/json"},
    )
    assert response.status == 415


def test_an_operation_with_no_body_refuses_one(harness: Harness) -> None:
    response = harness.client.request(
        "DELETE",
        "/matter/commissioning",
        raw=b'{"force": true}',
        headers={"content-type": "application/json"},
    )
    assert response.status == 415


# -- routing --------------------------------------------------------------


def test_an_unknown_api_path_is_a_json_404(harness: Harness) -> None:
    """Never an SPA redirect. The rule keeps a mistyped endpoint from arriving at
    the frontend as an HTML page that its JSON parser chokes on."""
    response = harness.client.get("/nothing/here")
    assert response.status == 404
    assert response.headers["Content-Type"] == "application/json"
    assert response.json["error"]["code"] == "not_found"


def test_a_path_outside_the_api_base_is_also_a_json_404(harness: Harness) -> None:
    response = harness.app.handle(Request("GET", "/index.html"))
    assert response.status == 404
    assert response.json["error"]["code"] == "not_found"


def test_a_method_the_document_does_not_define_is_a_404_naming_it(harness: Harness) -> None:
    """There is no "method not allowed" in the contract's table, and the rule for
    an unknown API URL is a JSON 404 — so it is one, with the method in the
    message so the cause is not a mystery."""
    response = harness.client.request("PUT", "/system/status", {})
    assert response.status == 404
    assert "PUT" in response.json["error"]["message"]


def test_every_response_carries_the_headers_the_contract_requires(harness: Harness) -> None:
    for response in (
        harness.client.get("/system/status"),
        harness.client.get("/nothing"),
        harness.client.delete("/auth/session"),
    ):
        assert response.headers["Cache-Control"] == "no-store"
        assert response.headers["X-Request-ID"].startswith("req_")


def test_request_ids_are_unique_per_request(harness: Harness) -> None:
    seen = {harness.client.get("/system/status").headers["X-Request-ID"] for _ in range(20)}
    assert len(seen) == 20


def test_an_accepted_response_carries_location_and_retry_after(harness: Harness) -> None:
    response = harness.client.post("/network/wifi/scans", {})
    assert response.headers["Location"] == f"/api/v1/jobs/{response.json['job_id']}"
    assert response.headers["Retry-After"].isdigit()
