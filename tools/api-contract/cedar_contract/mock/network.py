# SPDX-License-Identifier: Apache-2.0
"""Runtime status, the confirmed configuration, and the apply transaction.

`GET /network/status` and `GET /network/config` are deliberately two resources,
as the contract insists: one is what the interfaces are doing now, the other is
what was last confirmed plus its revision. A frontend that conflates them writes
a settings form that cannot tell a DHCP lease from a configured address.

The transaction is the part worth mocking properly. `staged -> applying ->
awaiting_confirmation -> committed` with a rollback branch is the only flow in
this API where the client can lose the connection it is being reconfigured
through, and every one of its edges — a stale revision, a candidate that
expired, a confirmation that never came — is a screen someone has to build.

P4 brought the device's network-manager to these rules and, where the device
had a reason to differ, this module to the device's: the confirmation deadline
starts at apply, a Wi-Fi coprocessor that is not ready refuses Wi-Fi, a confirm
is refused while the changed interfaces are not working yet, and an address is
judged by the same host rule as `api_ipv4_is_usable_host()` in C.
"""

from __future__ import annotations

import ipaddress
from dataclasses import dataclass
from typing import Any

from ..errors import ApiError, error
from .clock import Clock
from .constants import (
    CANDIDATE_TTL_MS,
    DEFAULT_CONFIG,
    NETWORK_APPLY_MS,
    NETWORK_COMMIT_MS,
    NETWORK_DISCARD_MS,
    NETWORK_ROLLBACK_MS,
    QUEUE_MS,
)
from .jobs import JobStore, Step
from .scenario import Scenario
from .util import decode_ssid, deep_copy, detail, ssid_text


@dataclass
class Transaction:
    id: str
    base_revision: int
    candidate: dict[str, Any]
    state: str
    created_ms: int
    job_id: str | None = None
    deadline_ms: int | None = None
    error: ApiError | None = None


class Network:
    """Status, the confirmed configuration, and the apply transaction."""

    def __init__(self, clock: Clock, jobs: JobStore, scenario: Scenario) -> None:
        self._clock = clock
        self._jobs = jobs
        self._scenario = scenario
        self.revision = 1
        self.config: dict[str, Any] = deep_copy(DEFAULT_CONFIG)
        self.transaction: Transaction | None = None
        self._stored_password = False
        self._counter = 0

    # -- reads -----------------------------------------------------------

    def config_json(self) -> dict[str, object]:
        self.settle()
        pending = self.transaction
        return {
            "revision": self.revision,
            "config": deep_copy(self.config),
            "pending_transaction_id": pending.id
            if pending is not None and pending.state in _PENDING_STATES
            else None,
        }

    def wifi_radio_error(self) -> ApiError | None:
        """Why Wi-Fi cannot be used right now, or None when it can.

        The radio is the coprocessor, so its state is the coprocessor's. The
        device reports the same thing when the C6 does not answer ESP-Hosted at
        boot — an empty flash, a failed update — and the error is carried even
        while Wi-Fi is disabled, because that is when the screen has to explain
        why enabling it is refused.
        """
        state = self._scenario.coprocessor_state
        if state == "ready":
            return None
        return error("capability_unavailable", f"The Wi-Fi coprocessor is {state}")

    def status_json(self) -> dict[str, object]:
        eth = self.config["interfaces"]["ethernet"]
        wifi = self.config["interfaces"]["wifi"]
        radio_error = self.wifi_radio_error()
        wifi_up = wifi["enabled"] and radio_error is None
        if not wifi["enabled"]:
            wifi_state = "disabled"
        elif radio_error is not None:
            wifi_state = "failed"
        else:
            wifi_state = "ready"
        interfaces = [
            {
                "id": "ethernet",
                "enabled": eth["enabled"],
                "link_up": eth["enabled"],
                "state": "ready" if eth["enabled"] else "disabled",
                "mac_address": "02:00:5e:10:12:73",
                "addresses": _addresses_for(eth["ipv4"], "192.168.88.14")
                + [
                    {
                        "family": "ipv6",
                        "address": "fe80::8234:28ff:fe10:1273",
                        "prefix_length": 64,
                        "source": "link_local",
                    }
                ]
                if eth["enabled"]
                else [],
                "ssid": None,
                "rssi_dbm": None,
                "error": None,
            },
            {
                "id": "wifi",
                "enabled": wifi["enabled"],
                "link_up": wifi_up,
                "state": wifi_state,
                "mac_address": "02:00:5e:10:12:74",
                "addresses": _addresses_for(wifi["ipv4"], "192.168.88.21") if wifi_up else [],
                "ssid": ssid_text(wifi["ssid_base64"]) if wifi_up else None,
                "rssi_dbm": -58 if wifi_up else None,
                "error": None if radio_error is None else detail(radio_error),
            },
        ]
        return {
            "interfaces": interfaces,
            "default_interface": self.config["preferred_interface"]
            if any(i["link_up"] for i in interfaces)
            else None,
            "dns_servers": list(self.config["dns"]["servers"]) or ["192.168.88.1"],
        }

    def transaction_json(self, tx: Transaction) -> dict[str, object]:
        self.settle()
        remaining: int | None
        if tx.state == "staged":
            remaining = max(0, (tx.created_ms + CANDIDATE_TTL_MS - self._clock.now_ms()) // 1000)
        elif tx.state in ("applying", "awaiting_confirmation") and tx.deadline_ms is not None:
            remaining = max(0, (tx.deadline_ms - self._clock.now_ms()) // 1000)
        else:
            remaining = None
        return {
            "id": tx.id,
            "boot_id": self._jobs.boot_id,
            "base_revision": tx.base_revision,
            "state": tx.state,
            "candidate": deep_copy(tx.candidate),
            "remaining_seconds": remaining,
            "reconnect_urls": _reconnect_urls(tx.candidate)
            if tx.state in ("applying", "awaiting_confirmation")
            else [],
            "job_id": tx.job_id,
            "error": None if tx.error is None else detail(tx.error),
        }

    def find(self, transaction_id: str) -> Transaction:
        self.settle()
        if self.transaction is None or self.transaction.id != transaction_id:
            raise error("not_found", "No such network transaction")
        return self.transaction

    def change_in_progress(self) -> bool:
        """A change the radio must not be taken away from: scanning waits."""
        tx = self.transaction
        return tx is not None and tx.state in ("applying", "awaiting_confirmation", "rolling_back")

    # -- transitions -----------------------------------------------------

    def stage(self, body: dict[str, Any]) -> Transaction:
        self.settle()
        if self.transaction is not None and self.transaction.state in _PENDING_STATES:
            raise error(
                "busy",
                "Another network transaction is in progress; discard it first",
            )
        if body["base_revision"] != self.revision:
            raise error(
                "stale_revision",
                f"The configuration is at revision {self.revision}; re-read it and stage again",
            )
        candidate = self._validate(body["config"])
        self._counter += 1
        self.transaction = Transaction(
            id=f"nettx_{self._counter:04x}",
            base_revision=body["base_revision"],
            candidate=candidate,
            state="staged",
            created_ms=self._clock.now_ms(),
        )
        return self.transaction

    def apply(self, tx: Transaction, timeout_seconds: int) -> str:
        """Journal, answer, then change the network.

        DEVICE RULE. The confirmation deadline starts here, not when the
        interfaces have been changed: a worker that never runs, or stops half
        way, must still end in a rollback instead of leaving the device on a
        configuration nobody confirmed. So the countdown is visible while the
        change is still being applied.
        """
        if tx.state != "staged":
            raise error("invalid_state", f"A transaction in state {tx.state!r} cannot be applied")

        def applied() -> None:
            if tx.state == "applying":
                tx.state = "awaiting_confirmation"

        job = self._jobs.create(
            "network_apply",
            [
                Step("queued", QUEUE_MS, state="queued"),
                Step("applying", NETWORK_APPLY_MS, "steps", 3, on_done=applied),
            ],
            resource_url="/api/v1/network/config",
            park_state="waiting_confirmation",
        )
        tx.state = "applying"
        tx.job_id = job.id
        tx.deadline_ms = self._clock.now_ms() + timeout_seconds * 1000
        return job.id

    def confirm(self, tx: Transaction) -> str:
        if tx.state != "awaiting_confirmation":
            raise error(
                "invalid_state",
                f"A transaction in state {tx.state!r} cannot be confirmed",
            )
        if self._scenario.network_health == "unhealthy":
            # DEVICE RULE. Reaching this endpoint proves only the client's own
            # path; the device checks link, address and route of every enabled
            # interface first, and a DHCP lease that has not arrived yet is
            # refused rather than committed.
            raise error(
                "invalid_state",
                "Not every enabled interface is working yet; confirm again shortly",
            )
        job = self._jobs.get(tx.job_id or "")
        if job is None:
            raise error("internal_error", "The apply job is gone")

        def committed() -> None:
            self.config = deep_copy(tx.candidate)
            # What `keep` is judged against from now on: the store derives
            # password_set from the secret that survived the commit.
            self._stored_password = bool(tx.candidate["interfaces"]["wifi"]["password_set"])
            self.revision += 1
            tx.state = "committed"
            tx.deadline_ms = None

        job.resume(
            self._clock.now_ms(),
            [Step("committing", NETWORK_COMMIT_MS, "steps", 2, on_done=committed)],
        )
        return job.id

    def discard(self, tx: Transaction) -> str:
        """`DELETE` means discard a candidate or roll back an applied one.

        The contract splits them: a staged candidate gets its own short job, an
        applied one reuses the apply job so there is never a second network
        operation competing with the first.
        """
        if tx.state == "staged":

            def discarded() -> None:
                tx.state = "rolled_back"

            job = self._jobs.create(
                "network_discard",
                [Step("discarding", NETWORK_DISCARD_MS, on_done=discarded)],
                resource_url="/api/v1/network/config",
            )
            tx.job_id = job.id
            return job.id
        if tx.state in ("applying", "awaiting_confirmation"):
            return self._roll_back(tx, None)
        raise error("invalid_state", f"A transaction in state {tx.state!r} cannot be rolled back")

    def settle(self) -> None:
        """Expiry and the confirmation timeout, the two transitions nobody asks
        for."""
        tx = self.transaction
        if tx is None:
            return
        now = self._clock.now_ms()
        if tx.state == "staged" and now - tx.created_ms >= CANDIDATE_TTL_MS:
            tx.state = "expired"
            tx.error = error(
                "resource_expired",
                "The candidate configuration expired before it was applied",
            )
            return
        if (
            tx.state in ("applying", "awaiting_confirmation")
            and tx.deadline_ms is not None
            and now >= tx.deadline_ms
        ):
            self._roll_back(
                tx,
                error(
                    "resource_expired",
                    "Confirmation timed out; the previous configuration was restored",
                ),
            )

    def _roll_back(self, tx: Transaction, cause: ApiError | None) -> str:
        """Rollback shares the apply job, and fails it when nobody confirmed.

        DECISION. The contract does not name a code for a confirmation timeout.
        `resource_expired` is used: the transaction is exactly the
        non-transferable resource the 410 row describes, and the client's
        recovery — re-read `network/config` and stage again — is the recovery
        that code implies. `busy` and `invalid_state` would both suggest
        retrying the same confirm, which can never work.
        """
        job = self._jobs.get(tx.job_id or "")
        tx.error = cause

        def rolled_back() -> None:
            tx.state = "rolled_back"
            tx.deadline_ms = None

        steps = [Step("rolling_back", NETWORK_ROLLBACK_MS, "steps", 2, on_done=rolled_back)]
        tx.state = "rolling_back"
        if job is None:
            job = self._jobs.create("network_apply", steps, resource_url="/api/v1/network/config")
            tx.job_id = job.id
            return job.id
        if job.parked:
            job.resume(
                self._clock.now_ms(),
                steps,
                final_state="succeeded" if cause is None else "failed",
            )
        else:
            job.steps.extend(steps)
            job.final_state = "succeeded" if cause is None else "failed"
        if cause is not None:
            job.error = cause
        return job.id

    # -- candidate validation -------------------------------------------

    def _validate(self, config: dict[str, Any]) -> dict[str, Any]:
        """The business rules schema cannot express, from section 5 and the
        network part of the contract.

        These duplicate no schema keyword: every one of them is a relationship
        between two fields, which is exactly the class of rule the plan puts in
        the service rather than in the document.
        """
        err = ApiError(code="validation_failed", message="The configuration cannot be applied")
        eth = config["interfaces"]["ethernet"]
        wifi = config["interfaces"]["wifi"]

        if not eth["enabled"] and not wifi["enabled"]:
            err.add_field("/config/interfaces/ethernet/enabled", "conflicting")
            err.add_field("/config/interfaces/wifi/enabled", "conflicting")
        elif wifi["enabled"] and self.wifi_radio_error() is not None:
            # DEVICE RULE. A Wi-Fi interface that cannot come up would make
            # every change unconfirmable — confirm waits for each enabled
            # interface — so it is refused at the field, while the device is
            # still reachable and the operator can see why.
            err.add_field("/config/interfaces/wifi/enabled", "not_allowed")

        for name, iface in (("ethernet", eth), ("wifi", wifi)):
            base = f"/config/interfaces/{name}/ipv4"
            self._check_ipv4(err, base, iface["ipv4"])

        dns = config["dns"]
        if dns["mode"] == "automatic" and dns["servers"]:
            err.add_field("/config/dns/servers", "not_allowed")
        if dns["mode"] == "manual" and not dns["servers"]:
            err.add_field("/config/dns/servers", "required")
        for index, server in enumerate(dns["servers"]):
            if ipaddress.ip_address(server).is_unspecified:
                # DEVICE RULE. 0.0.0.0 and :: pass the schema's formats and are
                # never a resolver.
                err.add_field(f"/config/dns/servers/{index}", "invalid_format")

        if config["preferred_interface"] == "wifi" and not wifi["enabled"]:
            err.add_field("/config/preferred_interface", "conflicting")
        if config["preferred_interface"] == "ethernet" and not eth["enabled"]:
            err.add_field("/config/preferred_interface", "conflicting")

        self._check_wifi(err, wifi)

        if err.fields:
            raise err
        return _redact(config, self._stored_password or _sets_password(wifi))

    def _check_ipv4(self, err: ApiError, base: str, ipv4: dict[str, Any]) -> None:
        if ipv4["mode"] == "dhcp":
            for field_name in ("address", "prefix_length", "gateway"):
                if ipv4[field_name] is not None:
                    err.add_field(f"{base}/{field_name}", "not_allowed")
            return
        if ipv4["address"] is None:
            err.add_field(f"{base}/address", "required")
        if ipv4["prefix_length"] is None:
            err.add_field(f"{base}/prefix_length", "required")
        if ipv4["address"] is None or ipv4["prefix_length"] is None:
            return
        try:
            iface = ipaddress.IPv4Interface(f"{ipv4['address']}/{ipv4['prefix_length']}")
        except ValueError:
            err.add_field(f"{base}/address", "invalid_format")
            return
        if not _usable_host(iface.ip, iface.network):
            # The network and broadcast addresses of the prefix are not hosts,
            # and a device configured with one is a device nobody can reach.
            err.add_field(f"{base}/address", "out_of_range")
            # DEVICE RULE. A gateway is not judged against an address that is
            # already wrong: the second message would be noise.
            return
        if ipv4["gateway"] is not None:
            try:
                gateway = ipaddress.IPv4Address(ipv4["gateway"])
            except ValueError:
                err.add_field(f"{base}/gateway", "invalid_format")
                return
            if gateway not in iface.network or not _usable_host(gateway, iface.network):
                # An isolated LAN with no router is allowed, per the contract;
                # a gateway outside the prefix is not reachable by definition.
                err.add_field(f"{base}/gateway", "out_of_range")
            elif gateway == iface.ip:
                err.add_field(f"{base}/gateway", "conflicting")

    def _check_wifi(self, err: ApiError, wifi: dict[str, Any]) -> None:
        credential = wifi["credential"]
        action = credential["action"]
        if not wifi["enabled"]:
            return
        decoded = decode_ssid(wifi["ssid_base64"])
        if decoded is None:
            err.add_field("/config/interfaces/wifi/ssid_base64", "invalid_format")
        elif not decoded:
            err.add_field("/config/interfaces/wifi/ssid_base64", "required")
        elif len(decoded) > 32:
            err.add_field("/config/interfaces/wifi/ssid_base64", "out_of_range")
        if wifi["security"] == "open":
            if action == "replace":
                err.add_field("/config/interfaces/wifi/credential/action", "not_allowed")
        else:
            if action == "clear":
                err.add_field("/config/interfaces/wifi/credential/action", "conflicting")
            elif action == "keep" and not self._stored_password:
                # Keeping a password that was never set leaves an enabled
                # protected network with no way to associate.
                err.add_field("/config/interfaces/wifi/credential/action", "conflicting")
            elif action == "keep" and self._changes_profile(wifi):
                # The contract refuses `keep` across an SSID or security change:
                # the stored secret belongs to the old profile.
                err.add_field("/config/interfaces/wifi/credential/action", "not_allowed")

    def _changes_profile(self, wifi: dict[str, Any]) -> bool:
        current = self.config["interfaces"]["wifi"]
        return (
            wifi["ssid_base64"] != current["ssid_base64"]
            or wifi["security"] != current["security"]
        )


_PENDING_STATES = frozenset({"staged", "applying", "awaiting_confirmation", "rolling_back"})


def _usable_host(address: ipaddress.IPv4Address, network: ipaddress.IPv4Network) -> bool:
    """`api_ipv4_is_usable_host()` from api_validation.c, rule for rule.

    Besides the prefix's own network and broadcast addresses: 0.0.0.0/8,
    loopback, multicast and class E, and link-local — each accepted by an IP
    stack and then simply not working.
    """
    if address in (network.network_address, network.broadcast_address):
        return False
    first = address.packed[0]
    if first == 0 or first == 127 or first >= 224:
        return False
    return not (first == 169 and address.packed[1] == 254)


def _sets_password(wifi: dict[str, Any]) -> bool:
    """A replaced password is stored whether or not Wi-Fi is enabled, as the
    device's store does: a profile can be prepared and switched on later."""
    return wifi["credential"]["action"] == "replace"


def _redact(config: dict[str, Any], password_set: bool) -> dict[str, Any]:
    """`NetworkConfigInput` -> `NetworkConfigOutput`.

    The credential object is replaced by one boolean. This is the only
    transformation between the two schemas, and it is the reason they are two
    schemas: there is no request in which the server may echo a password back.
    """
    out = deep_copy(config)
    wifi = out["interfaces"]["wifi"]
    action = wifi.pop("credential")["action"]
    wifi["password_set"] = False if action == "clear" else password_set
    return out


def _addresses_for(ipv4: dict[str, Any], dhcp_address: str) -> list[dict[str, object]]:
    if ipv4["mode"] == "static" and ipv4["address"]:
        return [
            {
                "family": "ipv4",
                "address": ipv4["address"],
                "prefix_length": ipv4["prefix_length"] or 24,
                "source": "static",
            }
        ]
    return [{"family": "ipv4", "address": dhcp_address, "prefix_length": 24, "source": "dhcp"}]


def _reconnect_urls(candidate: dict[str, Any]) -> list[str]:
    """Where the UI should look for the device after the address changes.

    DEVICE RULE. Only addresses the candidate names: a static IPv4 of an
    enabled interface. A DHCP address is not known until the lease arrives, and
    the device has no name it can promise — its only mDNS responder is Matter's,
    which does not advertise the web interface — so a DHCP change yields no URL
    and the UI says to look the device up in the router.
    """
    urls: list[str] = []
    for iface in ("ethernet", "wifi"):
        config = candidate["interfaces"][iface]
        if not config["enabled"]:
            continue
        ipv4 = config["ipv4"]
        if ipv4["mode"] == "static" and ipv4["address"]:
            urls.append(f"http://{ipv4['address']}/")
    return urls[:8]
