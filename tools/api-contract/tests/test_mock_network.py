# SPDX-License-Identifier: Apache-2.0
"""The network transaction, every edge of it.

This is the one flow in the API where the client can lose the connection it is
being reconfigured through, which is why the contract gives it a confirmation
timer and a rollback rather than a plain PUT. A frontend has to handle
`staged -> applying -> awaiting_confirmation -> committed`, the rollback branch,
a candidate that expired, a revision that moved under it and a confirm that
arrived too late — so all of them are here.

The business rules are the second half. None of them is expressible in the
schema: each is a relationship between two fields, which is exactly the class of
rule section 12 puts in the service's own tests. The rules and their field codes
are checked against what the frontend needs to render, not just for a 422.
"""

from __future__ import annotations

from typing import Any

from cedar_contract.mock.constants import (
    CANDIDATE_TTL_MS,
    NETWORK_APPLY_MS,
    NETWORK_COMMIT_MS,
    NETWORK_DISCARD_MS,
    NETWORK_ROLLBACK_MS,
    QUEUE_MS,
)

from cedar_contract.openapi import Document

from .conftest import VALID_CANDIDATE, Harness, build, candidate

SECOND = 1 / 1000
APPLIED = (QUEUE_MS + NETWORK_APPLY_MS) * SECOND + 0.1


def stage(
    harness: Harness, config: dict[str, Any] | None = None, revision: int | None = None
) -> Any:
    if revision is None:
        revision = harness.client.get("/network/config").json["revision"]
    return harness.client.post(
        "/network/transactions",
        {"base_revision": revision, "config": config or VALID_CANDIDATE},
    )


# -- the two resources are two resources ----------------------------------


def test_status_and_config_are_different_resources(harness: Harness) -> None:
    """One is what the interfaces are doing, the other is what was confirmed. A
    frontend that conflates them cannot tell a DHCP lease from a configuration."""
    status = harness.client.get("/network/status").json
    config = harness.client.get("/network/config").json
    assert {iface["id"] for iface in status["interfaces"]} == {"ethernet", "wifi"}
    assert set(config) == {"revision", "config", "pending_transaction_id"}
    assert "revision" not in status


def test_a_dhcp_address_is_reported_with_its_source(harness: Harness) -> None:
    ethernet = harness.client.get("/network/status").json["interfaces"][0]
    assert ethernet["addresses"][0]["source"] == "dhcp"
    assert any(a["family"] == "ipv6" for a in ethernet["addresses"]), (
        "IPv6 stays on with automatic configuration, and its addresses are shown"
    )


def test_no_secret_appears_in_any_network_response(harness: Harness) -> None:
    config = candidate(
        **{
            "interfaces/wifi/enabled": True,
            "interfaces/wifi/ssid_base64": "Q2VkYXItTGFi",
            "interfaces/wifi/security": "wpa2_psk",
            "interfaces/wifi/credential": {"action": "replace", "value": "sup3r-s3cret-pw"},
        }
    )
    staged = stage(harness, config)
    assert staged.status == 201
    assert "sup3r-s3cret-pw" not in staged.body.decode()
    wifi = staged.json["candidate"]["interfaces"]["wifi"]
    assert wifi["password_set"] is True
    assert "credential" not in wifi, "the output schema has no such field"


# -- the happy path -------------------------------------------------------


def test_a_candidate_is_staged_with_a_countdown_and_a_location(harness: Harness) -> None:
    staged = stage(harness)
    assert staged.status == 201
    assert staged.headers["Location"] == f"/api/v1/network/transactions/{staged.json['id']}"
    assert staged.json["state"] == "staged"
    assert staged.json["remaining_seconds"] == CANDIDATE_TTL_MS // 1000
    assert staged.json["reconnect_urls"] == [], "nothing to reconnect to before apply"
    assert (
        harness.client.get("/network/config").json["pending_transaction_id"] == staged.json["id"]
    )


def test_the_full_transaction_reaches_committed_and_bumps_the_revision(harness: Harness) -> None:
    before = harness.client.get("/network/config").json["revision"]
    staged = stage(harness)
    transaction = staged.json["id"]

    accepted = harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    assert accepted.status == 202
    job = accepted.json["job_id"]
    assert harness.client.get(f"/network/transactions/{transaction}").json["state"] == "applying"

    harness.advance(APPLIED)
    applying = harness.client.get(f"/network/transactions/{transaction}").json
    assert applying["state"] == "awaiting_confirmation"
    assert applying["remaining_seconds"] == 116, "the deadline started at apply"
    assert applying["reconnect_urls"] == ["http://192.168.88.50/"]
    assert harness.client.get("/network/config").json["revision"] == before, (
        "nothing is committed until it is confirmed"
    )

    confirmed = harness.client.post(f"/network/transactions/{transaction}/confirm", {})
    assert confirmed.json["job_id"] == job, (
        "confirm answers with the same job: there is never a second competing network operation"
    )
    harness.advance(NETWORK_COMMIT_MS * SECOND + 0.1)

    after = harness.client.get("/network/config").json
    assert after["revision"] == before + 1
    assert after["pending_transaction_id"] is None
    assert after["config"]["interfaces"]["ethernet"]["ipv4"]["address"] == "192.168.88.50"
    assert harness.client.get(f"/network/transactions/{transaction}").json["state"] == "committed"
    assert harness.client.get(f"/jobs/{job}").json["state"] == "succeeded"


def test_the_committed_configuration_shows_in_the_runtime_status(harness: Harness) -> None:
    transaction = stage(harness).json["id"]
    harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    harness.advance(APPLIED)
    harness.client.post(f"/network/transactions/{transaction}/confirm", {})
    harness.advance(NETWORK_COMMIT_MS * SECOND + 0.1)
    ethernet = harness.client.get("/network/status").json["interfaces"][0]
    assert ethernet["addresses"][0] == {
        "family": "ipv4",
        "address": "192.168.88.50",
        "prefix_length": 24,
        "source": "static",
    }


# -- the edges ------------------------------------------------------------


def test_a_stale_base_revision_is_refused_with_stale_revision(harness: Harness) -> None:
    """Not retryable: repeating a request built on a revision that has moved on
    can never work, and the client has to re-read first."""
    response = stage(harness, revision=99)
    assert response.status == 409
    assert response.json["error"]["code"] == "stale_revision"
    assert response.json["error"]["retryable"] is False
    assert "revision 1" in response.json["error"]["message"], "it says what to re-read"


def test_two_clients_with_different_revisions_cannot_both_win(harness: Harness) -> None:
    """The acceptance matrix names this one. The second client is working from a
    revision that the first has already moved."""
    revision = harness.client.get("/network/config").json["revision"]
    transaction = stage(harness, revision=revision).json["id"]
    harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    harness.advance(APPLIED)
    harness.client.post(f"/network/transactions/{transaction}/confirm", {})
    harness.advance(NETWORK_COMMIT_MS * SECOND + 0.1)
    second = stage(harness, revision=revision)
    assert second.status == 409
    assert second.json["error"]["code"] == "stale_revision"


def test_only_one_candidate_exists_at_a_time(harness: Harness) -> None:
    stage(harness)
    second = stage(harness)
    assert second.status == 409
    assert second.json["error"]["code"] == "busy"
    assert second.json["error"]["retryable"] is True, "a conflict of timing may clear"


def test_a_staged_candidate_expires(harness: Harness) -> None:
    transaction = stage(harness).json["id"]
    harness.advance(CANDIDATE_TTL_MS * SECOND - 1)
    still = harness.client.get(f"/network/transactions/{transaction}").json
    assert still["state"] == "staged" and still["remaining_seconds"] == 1
    harness.advance(2)
    expired = harness.client.get(f"/network/transactions/{transaction}").json
    assert expired["state"] == "expired"
    assert expired["error"]["code"] == "resource_expired"
    assert expired["remaining_seconds"] is None
    assert harness.client.get("/network/config").json["pending_transaction_id"] is None


def test_applying_an_expired_candidate_is_invalid_state(harness: Harness) -> None:
    transaction = stage(harness).json["id"]
    harness.advance(CANDIDATE_TTL_MS * SECOND + 1)
    response = harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    assert response.status == 409
    assert response.json["error"]["code"] == "invalid_state"


def test_confirming_before_apply_is_invalid_state(harness: Harness) -> None:
    transaction = stage(harness).json["id"]
    response = harness.client.post(f"/network/transactions/{transaction}/confirm", {})
    assert response.status == 409
    assert "staged" in response.json["error"]["message"]


def test_a_confirmation_that_never_comes_rolls_back(harness: Harness) -> None:
    """The timeout the contract puts at 120 seconds by default, and the reason the
    clock here can be skipped: a test that waited it out would not be run."""
    before = harness.client.get("/network/config").json
    transaction = stage(harness).json["id"]
    accepted = harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    job = accepted.json["job_id"]
    harness.advance(APPLIED)
    harness.advance(121)

    rolling = harness.client.get(f"/network/transactions/{transaction}").json
    assert rolling["state"] == "rolling_back"
    assert rolling["error"]["code"] == "resource_expired"
    assert harness.client.get(f"/jobs/{job}").json["state"] == "running"

    harness.advance(NETWORK_ROLLBACK_MS * SECOND + 0.1)
    assert (
        harness.client.get(f"/network/transactions/{transaction}").json["state"] == "rolled_back"
    )
    assert harness.client.get(f"/jobs/{job}").json["state"] == "failed"
    after = harness.client.get("/network/config").json
    assert after["revision"] == before["revision"]
    assert after["config"] == before["config"], "the previous configuration is restored"
    assert after["pending_transaction_id"] is None


def test_confirming_after_the_timeout_is_refused(harness: Harness) -> None:
    transaction = stage(harness).json["id"]
    harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 60}
    )
    harness.advance(APPLIED)
    harness.advance(61)
    response = harness.client.post(f"/network/transactions/{transaction}/confirm", {})
    assert response.status == 409
    assert response.json["error"]["code"] == "invalid_state"


def test_deleting_a_staged_candidate_discards_it_with_its_own_job(harness: Harness) -> None:
    """The contract splits the two: a staged candidate gets a short job of its
    own, an applied one reuses the apply job."""
    transaction = stage(harness).json["id"]
    accepted = harness.client.delete(f"/network/transactions/{transaction}")
    assert accepted.status == 202
    job = harness.client.get(f"/jobs/{accepted.json['job_id']}").json
    assert job["kind"] == "network_discard"
    harness.advance(NETWORK_DISCARD_MS * SECOND + 0.1)
    assert (
        harness.client.get(f"/network/transactions/{transaction}").json["state"] == "rolled_back"
    )


def test_deleting_an_applied_transaction_rolls_back_on_the_same_job(harness: Harness) -> None:
    transaction = stage(harness).json["id"]
    accepted = harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    harness.advance(APPLIED)
    rolled = harness.client.delete(f"/network/transactions/{transaction}")
    assert rolled.json["job_id"] == accepted.json["job_id"]
    harness.advance(NETWORK_ROLLBACK_MS * SECOND + 0.1)
    assert (
        harness.client.get(f"/network/transactions/{transaction}").json["state"] == "rolled_back"
    )
    assert harness.client.get(f"/jobs/{rolled.json['job_id']}").json["state"] == "succeeded", (
        "a rollback the administrator asked for is a success, not a failure"
    )


def test_a_rollback_asked_for_while_applying_is_not_undone_by_the_apply_finishing(
    harness: Harness,
) -> None:
    """DELETE during `applying` starts the rollback on the apply job. The apply
    finishing afterwards must not move the transaction on to
    `awaiting_confirmation`: a client could then confirm a change the
    administrator has already withdrawn."""
    transaction = stage(harness).json["id"]
    accepted = harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    assert harness.client.get(f"/network/transactions/{transaction}").json["state"] == "applying"
    rolled = harness.client.delete(f"/network/transactions/{transaction}")
    assert rolled.status == 202 and rolled.json["job_id"] == accepted.json["job_id"]

    harness.advance(APPLIED)
    assert (
        harness.client.get(f"/network/transactions/{transaction}").json["state"] == "rolling_back"
    ), "the apply step finished, and the rollback is still what is happening"
    assert harness.client.post(f"/network/transactions/{transaction}/confirm", {}).status == 409

    harness.advance(NETWORK_ROLLBACK_MS * SECOND + 0.1)
    assert (
        harness.client.get(f"/network/transactions/{transaction}").json["state"] == "rolled_back"
    )


def test_a_rolled_back_transaction_frees_the_slot(harness: Harness) -> None:
    transaction = stage(harness).json["id"]
    harness.client.delete(f"/network/transactions/{transaction}")
    harness.advance(NETWORK_DISCARD_MS * SECOND + 0.1)
    assert stage(harness).status == 201


def test_an_unknown_transaction_is_not_found(harness: Harness) -> None:
    assert harness.client.get("/network/transactions/nettx_ffff").status == 404


def test_the_confirmation_deadline_starts_at_apply(harness: Harness) -> None:
    """The device arms it when the change is journalled, so a worker that never
    finishes still ends in a rollback — and the countdown is there while the
    change is being applied, not only after."""
    transaction = stage(harness).json["id"]
    harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 60}
    )
    applying = harness.client.get(f"/network/transactions/{transaction}").json
    assert applying["state"] == "applying"
    assert applying["remaining_seconds"] == 60
    harness.advance(APPLIED)
    awaiting = harness.client.get(f"/network/transactions/{transaction}").json
    assert awaiting["remaining_seconds"] == 56


def test_a_disabled_interface_offers_no_reconnect_url(harness: Harness) -> None:
    """A static address kept on a disabled interface is a stored profile, not a
    place the device answers. Offering it would send the operator to an address
    nothing is listening on."""
    static = {"mode": "static", "address": "192.168.88.60", "prefix_length": 24, "gateway": None}
    staged = stage(harness, candidate(**{"interfaces/wifi/ipv4": static}))
    assert staged.status == 201, staged.body
    transaction = staged.json["id"]
    harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    assert harness.client.get(f"/network/transactions/{transaction}").json["reconnect_urls"] == [
        "http://192.168.88.50/"
    ], "only the enabled Ethernet's address"


def test_a_dhcp_address_offers_no_reconnect_url(harness: Harness) -> None:
    """A lease that has not been granted has no address to link to, and the device
    has no name it can promise."""
    dhcp = {"mode": "dhcp", "address": None, "prefix_length": None, "gateway": None}
    transaction = stage(harness, candidate(**{"interfaces/ethernet/ipv4": dhcp})).json["id"]
    harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    assert harness.client.get(f"/network/transactions/{transaction}").json["reconnect_urls"] == []


def test_a_confirm_is_refused_while_the_interfaces_are_not_working(document: Document) -> None:
    """The request reaching the device proves only the client's own path. Until
    every changed interface has its link and address the transaction stays open,
    and the same confirm succeeds once they do."""
    harness = build(document, setup_required=False, network_health="unhealthy")
    harness.client.login()
    transaction = stage(harness).json["id"]
    job = harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    ).json["job_id"]
    harness.advance(APPLIED)

    refused = harness.client.post(f"/network/transactions/{transaction}/confirm", {})
    assert refused.status == 409
    assert refused.json["error"]["code"] == "invalid_state"
    still = harness.client.get(f"/network/transactions/{transaction}").json
    assert still["state"] == "awaiting_confirmation" and still["error"] is None

    harness.app.state.scenario.network_health = "healthy"
    accepted = harness.client.post(f"/network/transactions/{transaction}/confirm", {})
    assert accepted.status == 202 and accepted.json["job_id"] == job


# -- business rules -------------------------------------------------------


def _fields(response: Any) -> list[dict[str, str]]:
    assert response.status == 422, response.body
    assert response.json["error"]["code"] == "validation_failed"
    return response.json["error"].get("fields", [])


def test_static_without_an_address_names_both_missing_fields(harness: Harness) -> None:
    response = stage(
        harness,
        candidate(
            **{
                "interfaces/ethernet/ipv4/address": None,
                "interfaces/ethernet/ipv4/prefix_length": None,
                "interfaces/ethernet/ipv4/gateway": None,
            }
        ),
    )
    assert _fields(response) == [
        {"path": "/config/interfaces/ethernet/ipv4/address", "code": "required"},
        {"path": "/config/interfaces/ethernet/ipv4/prefix_length", "code": "required"},
    ]


def test_dhcp_with_static_fields_set_is_refused(harness: Harness) -> None:
    """The contract requires null for the static fields under DHCP; accepting them
    would leave the UI unable to say which of the two it is looking at."""
    response = stage(harness, candidate(**{"interfaces/ethernet/ipv4/mode": "dhcp"}))
    assert _fields(response) == [
        {"path": "/config/interfaces/ethernet/ipv4/address", "code": "not_allowed"},
        {"path": "/config/interfaces/ethernet/ipv4/prefix_length", "code": "not_allowed"},
        {"path": "/config/interfaces/ethernet/ipv4/gateway", "code": "not_allowed"},
    ]


def test_a_gateway_outside_the_prefix_is_out_of_range(harness: Harness) -> None:
    response = stage(harness, candidate(**{"interfaces/ethernet/ipv4/gateway": "10.0.0.1"}))
    assert _fields(response) == [
        {"path": "/config/interfaces/ethernet/ipv4/gateway", "code": "out_of_range"}
    ]


def test_a_gateway_equal_to_the_device_address_is_conflicting(harness: Harness) -> None:
    response = stage(harness, candidate(**{"interfaces/ethernet/ipv4/gateway": "192.168.88.50"}))
    assert _fields(response) == [
        {"path": "/config/interfaces/ethernet/ipv4/gateway", "code": "conflicting"}
    ]


def test_an_isolated_lan_with_no_gateway_is_allowed(harness: Harness) -> None:
    """The contract says so: a LAN with no router is a legitimate installation,
    and refusing it would block one."""
    assert stage(harness, candidate(**{"interfaces/ethernet/ipv4/gateway": None})).status == 201


def test_the_network_and_broadcast_addresses_are_not_hosts(harness: Harness) -> None:
    for address in ("192.168.88.0", "192.168.88.255"):
        harness.app.state.network.transaction = None
        response = stage(harness, candidate(**{"interfaces/ethernet/ipv4/address": address}))
        assert _fields(response) == [
            {"path": "/config/interfaces/ethernet/ipv4/address", "code": "out_of_range"}
        ], address


def test_addresses_no_lan_host_can_hold_are_out_of_range(harness: Harness) -> None:
    """The host rule of `api_ipv4_is_usable_host()` in C: an IP stack accepts each of
    these and then the device simply does not work."""
    for address, prefix in (
        ("0.1.2.3", 8),
        ("127.0.0.5", 8),
        ("169.254.3.4", 16),
        ("224.0.0.5", 24),
        ("240.1.2.3", 8),
    ):
        harness.app.state.network.transaction = None
        response = stage(
            harness,
            candidate(
                **{
                    "interfaces/ethernet/ipv4/address": address,
                    "interfaces/ethernet/ipv4/prefix_length": prefix,
                    "interfaces/ethernet/ipv4/gateway": None,
                }
            ),
        )
        assert _fields(response) == [
            {"path": "/config/interfaces/ethernet/ipv4/address", "code": "out_of_range"}
        ], address


def test_a_gateway_that_is_the_network_address_is_out_of_range(harness: Harness) -> None:
    response = stage(harness, candidate(**{"interfaces/ethernet/ipv4/gateway": "192.168.88.0"}))
    assert _fields(response) == [
        {"path": "/config/interfaces/ethernet/ipv4/gateway", "code": "out_of_range"}
    ]


def test_a_gateway_is_not_judged_against_an_address_that_is_already_wrong(
    harness: Harness,
) -> None:
    response = stage(
        harness,
        candidate(
            **{
                "interfaces/ethernet/ipv4/address": "192.168.88.255",
                "interfaces/ethernet/ipv4/gateway": "10.0.0.1",
            }
        ),
    )
    assert _fields(response) == [
        {"path": "/config/interfaces/ethernet/ipv4/address", "code": "out_of_range"}
    ]


def test_an_unspecified_resolver_is_refused_by_index(harness: Harness) -> None:
    """0.0.0.0 and :: pass the schema's formats and are never a resolver."""
    response = stage(harness, candidate(**{"dns": {"mode": "manual", "servers": ["0.0.0.0", "::"]}}))
    assert _fields(response) == [
        {"path": "/config/dns/servers/0", "code": "invalid_format"},
        {"path": "/config/dns/servers/1", "code": "invalid_format"},
    ]


def test_a_malformed_credential_is_one_conflicting_field(harness: Harness) -> None:
    """CredentialChange is a oneOf. A body no branch accepts is one entry on the
    object, whatever is wrong inside it — which the device's decoder repeats
    with WEB_JSON_ONEOF rather than naming the member."""
    for credential in (
        {"action": "keep", "value": "stray"},
        {"action": "replace"},
        {"action": "sometimes"},
        {},
        "keep",
        None,
    ):
        harness.app.state.network.transaction = None
        response = stage(harness, candidate(**{"interfaces/wifi/credential": credential}))
        assert _fields(response) == [
            {"path": "/config/interfaces/wifi/credential", "code": "conflicting"}
        ], credential


def test_a_resolver_that_is_not_an_address_is_conflicting(harness: Harness) -> None:
    """A server is a oneOf of an IPv4 and an IPv6 address: neither branch takes
    a name or a number."""
    response = stage(
        harness, candidate(**{"dns": {"mode": "manual", "servers": ["resolver", 53]}})
    )
    assert _fields(response) == [
        {"path": "/config/dns/servers/0", "code": "conflicting"},
        {"path": "/config/dns/servers/1", "code": "conflicting"},
    ]


def test_wifi_is_refused_while_the_coprocessor_is_not_ready(document: Document) -> None:
    """Confirm waits for every enabled interface, so a Wi-Fi that cannot come up
    would make every change unconfirmable; it is refused at the field instead, and
    the status says why even while Wi-Fi is off."""
    harness = build(document, setup_required=False, coprocessor_state="offline")
    harness.client.login()
    wifi = harness.client.get("/network/status").json["interfaces"][1]
    assert wifi["state"] == "disabled"
    assert wifi["link_up"] is False
    assert wifi["error"]["code"] == "capability_unavailable"

    response = stage(
        harness,
        candidate(
            **{
                "interfaces/wifi/enabled": True,
                "interfaces/wifi/ssid_base64": "Q2VkYXItTGFi",
                "interfaces/wifi/security": "wpa2_psk",
                "interfaces/wifi/credential": {"action": "replace", "value": "correct-horse"},
            }
        ),
    )
    assert _fields(response) == [{"path": "/config/interfaces/wifi/enabled", "code": "not_allowed"}]


WIFI_PROFILE = {
    "interfaces/wifi/enabled": True,
    "interfaces/wifi/ssid_base64": "Q2VkYXItTGFi",
    "interfaces/wifi/security": "wpa2_psk",
    "interfaces/wifi/credential": {"action": "replace", "value": "correct-horse"},
}


def _commit(harness: Harness, config: dict[str, Any]) -> None:
    staged = stage(harness, config)
    assert staged.status == 201, staged.body
    transaction = staged.json["id"]
    harness.client.post(
        f"/network/transactions/{transaction}/apply", {"confirmation_timeout_seconds": 120}
    )
    harness.advance(APPLIED)
    assert harness.client.post(f"/network/transactions/{transaction}/confirm", {}).status == 202
    harness.advance(NETWORK_COMMIT_MS * SECOND + 0.1)
    assert harness.client.get(f"/network/transactions/{transaction}").json["state"] == "committed"


def test_an_enabled_wifi_whose_coprocessor_stops_is_failed_and_carries_nothing(
    harness: Harness,
) -> None:
    """The radio is the coprocessor. When it stops answering, an enabled Wi-Fi is
    `failed` with no link, and with Ethernet off nothing is the default interface —
    as the device reports it. A status that kept showing `ready` would hide the
    very fault the operator has to find."""
    _commit(
        harness,
        candidate(
            **WIFI_PROFILE,
            **{"preferred_interface": "wifi", "interfaces/ethernet/enabled": False},
        ),
    )
    before = harness.client.get("/network/status").json
    assert before["interfaces"][1]["state"] == "ready"
    assert before["default_interface"] == "wifi"

    harness.app.state.scenario.coprocessor_state = "offline"
    after = harness.client.get("/network/status").json
    wifi = after["interfaces"][1]
    assert wifi["enabled"] is True
    assert wifi["state"] == "failed", "enabled and unable to work is failed, not ready"
    assert wifi["link_up"] is False, "a radio that does not answer has no link"
    assert wifi["addresses"] == [] and wifi["ssid"] is None
    assert wifi["error"]["code"] == "capability_unavailable"
    assert after["default_interface"] is None, "no interface with a link carries traffic"


def test_keep_after_a_committed_password_is_accepted(harness: Harness) -> None:
    """`keep` is judged against what the last commit stored. Once a password is
    committed, a later change to the same network must be able to keep it without
    the operator typing it again."""
    _commit(harness, candidate(**WIFI_PROFILE))
    kept = stage(
        harness,
        candidate(**{**WIFI_PROFILE, "interfaces/wifi/credential": {"action": "keep"}}),
    )
    assert kept.status == 201, kept.body
    assert kept.json["candidate"]["interfaces"]["wifi"]["password_set"] is True


def test_a_password_replaced_on_a_disabled_wifi_is_stored(harness: Harness) -> None:
    """A profile can be prepared with Wi-Fi off and switched on later. The device's
    store keeps a replaced secret either way, so the candidate says it is set."""
    staged = stage(
        harness, candidate(**{**WIFI_PROFILE, "interfaces/wifi/enabled": False})
    )
    assert staged.status == 201, staged.body
    assert staged.json["candidate"]["interfaces"]["wifi"]["password_set"] is True


def test_both_interfaces_disabled_is_refused(harness: Harness) -> None:
    """The contract forbids it outright: there would be no way back in."""
    response = stage(
        harness,
        candidate(**{"interfaces/ethernet/enabled": False, "interfaces/wifi/enabled": False}),
    )
    codes = {f["path"]: f["code"] for f in _fields(response)}
    assert codes["/config/interfaces/ethernet/enabled"] == "conflicting"
    assert codes["/config/interfaces/wifi/enabled"] == "conflicting"


def test_preferring_a_disabled_interface_is_refused(harness: Harness) -> None:
    response = stage(
        harness,
        candidate(
            **{
                "preferred_interface": "wifi",
                "interfaces/wifi/enabled": False,
            }
        ),
    )
    assert {"path": "/config/preferred_interface", "code": "conflicting"} in _fields(response)


def test_automatic_dns_with_servers_listed_is_refused(harness: Harness) -> None:
    response = stage(harness, candidate(**{"dns": {"mode": "automatic", "servers": ["1.1.1.1"]}}))
    assert _fields(response) == [{"path": "/config/dns/servers", "code": "not_allowed"}]


def test_manual_dns_with_no_servers_is_refused(harness: Harness) -> None:
    response = stage(harness, candidate(**{"dns": {"mode": "manual", "servers": []}}))
    assert _fields(response) == [{"path": "/config/dns/servers", "code": "required"}]


def test_more_than_two_dns_servers_is_a_schema_rejection(harness: Harness) -> None:
    """The limit is in the document, so this one is caught by the middleware — and
    the point of the test is that it still arrives as the same error body."""
    response = stage(
        harness,
        candidate(**{"dns": {"mode": "manual", "servers": ["1.1.1.1", "8.8.8.8", "9.9.9.9"]}}),
    )
    assert _fields(response) == [{"path": "/config/dns/servers", "code": "out_of_range"}]


def test_an_enabled_protected_network_needs_a_password(harness: Harness) -> None:
    """`keep` with nothing stored leaves an enabled network with no way to
    associate — true of a first configuration, and invisible in the schema."""
    response = stage(
        harness,
        candidate(
            **{
                "interfaces/wifi/enabled": True,
                "interfaces/wifi/ssid_base64": "Q2VkYXItTGFi",
                "interfaces/wifi/security": "wpa2_psk",
                "interfaces/wifi/credential": {"action": "keep"},
            }
        ),
    )
    assert {
        "path": "/config/interfaces/wifi/credential/action",
        "code": "conflicting",
    } in _fields(response)


def test_an_open_network_takes_no_password(harness: Harness) -> None:
    response = stage(
        harness,
        candidate(
            **{
                "interfaces/wifi/enabled": True,
                "interfaces/wifi/ssid_base64": "Z3Vlc3Q=",
                "interfaces/wifi/security": "open",
                "interfaces/wifi/credential": {"action": "replace", "value": "pointless"},
            }
        ),
    )
    assert {
        "path": "/config/interfaces/wifi/credential/action",
        "code": "not_allowed",
    } in _fields(response)


def test_clearing_the_password_of_a_protected_network_is_refused(harness: Harness) -> None:
    response = stage(
        harness,
        candidate(
            **{
                "interfaces/wifi/enabled": True,
                "interfaces/wifi/ssid_base64": "Q2VkYXItTGFi",
                "interfaces/wifi/security": "wpa3_sae",
                "interfaces/wifi/credential": {"action": "clear"},
            }
        ),
    )
    assert {
        "path": "/config/interfaces/wifi/credential/action",
        "code": "conflicting",
    } in _fields(response)


def test_an_enabled_network_needs_an_ssid(harness: Harness) -> None:
    response = stage(
        harness,
        candidate(
            **{
                "interfaces/wifi/enabled": True,
                "interfaces/wifi/ssid_base64": "",
                "interfaces/wifi/security": "open",
                "interfaces/wifi/credential": {"action": "clear"},
            }
        ),
    )
    assert {"path": "/config/interfaces/wifi/ssid_base64", "code": "required"} in _fields(response)


def test_an_ssid_longer_than_32_bytes_is_out_of_range(harness: Harness) -> None:
    """44 base64 characters pass the schema's `maxLength`; 33 decoded bytes do
    not pass the rule, and only the decoded form can tell."""
    import base64

    response = stage(
        harness,
        candidate(
            **{
                "interfaces/wifi/enabled": True,
                "interfaces/wifi/ssid_base64": base64.b64encode(b"n" * 33).decode(),
                "interfaces/wifi/security": "open",
                "interfaces/wifi/credential": {"action": "clear"},
            }
        ),
    )
    assert {
        "path": "/config/interfaces/wifi/ssid_base64",
        "code": "out_of_range",
    } in _fields(response)


def test_a_wifi_candidate_that_satisfies_every_rule_is_accepted(harness: Harness) -> None:
    response = stage(
        harness,
        candidate(
            **{
                "preferred_interface": "wifi",
                "interfaces/wifi/enabled": True,
                "interfaces/wifi/ssid_base64": "Q2VkYXItTGFi",
                "interfaces/wifi/security": "wpa3_sae",
                "interfaces/wifi/hidden": True,
                "interfaces/wifi/credential": {"action": "replace", "value": "correct-horse"},
            }
        ),
    )
    assert response.status == 201
    wifi = response.json["candidate"]["interfaces"]["wifi"]
    assert wifi["hidden"] is True and wifi["password_set"] is True


def test_the_field_list_is_bounded_and_says_so(harness: Harness) -> None:
    """Six is the device's `CONFIG_API_VALIDATION_MAX_FIELDS`. A candidate wrong
    in more ways than that must report the truncation, or a client fixes six and
    is rejected again with no hint that more were waiting."""
    from cedar_contract.errors import MAX_FIELDS, TRUNCATION_NOTE

    response = stage(
        harness,
        candidate(
            **{
                "preferred_interface": "wifi",
                "dns": {"mode": "manual", "servers": []},
                "interfaces/ethernet/enabled": False,
                "interfaces/ethernet/ipv4/mode": "dhcp",
                "interfaces/wifi/enabled": False,
            }
        ),
    )
    detail = response.json["error"]
    assert len(detail["fields"]) == MAX_FIELDS
    assert detail["message"].endswith(TRUNCATION_NOTE)
