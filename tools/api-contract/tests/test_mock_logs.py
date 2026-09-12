# SPDX-License-Identifier: Apache-2.0
"""Log paging, filters, and the cursor the contract makes opaque.

Logs sit at the coverage level in section 12, so the ring's own hard parts — a
wrap, a stale cursor from a previous boot, a gap — belong to `log-store`'s sim
suite and are not reproduced. What is reproduced is the paging arithmetic, because
a cursor that never advanced would make the log screen's loop run forever, and
because the contract has three specific rules about it that a frontend must obey.
"""

from __future__ import annotations

import json

from .conftest import Harness


def test_both_sources_are_reported_with_their_counters(harness: Harness) -> None:
    items = harness.client.get("/logs/sources").json["items"]
    assert [source["id"] for source in items] == ["stm32", "esp32"]
    for source in items:
        assert source["dropped_count"].isdigit(), "a decimal string, not a number"
    assert items[1]["generation"] != items[0]["generation"], (
        "the C6 has its own generation, which changes when it resets"
    )


def test_a_page_without_a_cursor_returns_the_newest_records_oldest_first(harness: Harness) -> None:
    page = harness.client.get("/logs/records?limit=5").json
    assert len(page["items"]) == 5
    sequence = [int(record["seq"]) for record in page["items"]]
    assert sequence == sorted(sequence), "oldest first within the page"
    everything = harness.client.get("/logs/records?limit=100").json
    assert sequence[-1] == int(everything["items"][-1]["seq"]), "the newest tail"


def test_the_cursor_points_at_the_next_scan_position(harness: Harness) -> None:
    """The contract's cursor moves forward into records that do not exist yet.

    Without one the client gets the tail, and the cursor that comes back says
    where to resume — so the page after the tail is empty until something new is
    logged. That is live tailing, and it is the only direction the contract
    offers: history comes from the export, because the ring is bounded.
    """
    tail = harness.client.get("/logs/records?limit=5").json
    assert tail["has_more"] is False, "the tail is the newest, so nothing follows it"
    assert tail["next_cursor"]

    caught_up = harness.client.get(f"/logs/records?limit=5&cursor={tail['next_cursor']}").json
    assert caught_up["items"] == []
    assert caught_up["has_more"] is False
    assert caught_up["next_cursor"] == tail["next_cursor"], "resuming twice is the same place"


def test_the_cursor_is_opaque(harness: Harness) -> None:
    """Opaque on purpose: a client that read an index out of it would build paging
    on an internal detail, and the contract binds the cursor to the filters too."""
    cursor = harness.client.get("/logs/records?limit=5").json["next_cursor"]
    assert len(cursor) <= 512
    assert not cursor.isdigit()
    assert cursor != harness.client.get("/logs/records?limit=5&source=esp32").json["next_cursor"]


def test_paging_forward_delivers_each_record_once(harness: Harness) -> None:
    """Walked from a cursor the mock issues for the start of the buffer, which is
    the one thing a test can reach that a live client cannot: it is how "no record
    twice, none skipped" gets checked at all."""
    state = harness.state.logs
    cursor = state._encode_cursor(0, {})
    seen: list[str] = []
    for _ in range(20):
        page = harness.client.get(f"/logs/records?limit=7&cursor={cursor}").json
        seen.extend(record["seq"] for record in page["items"])
        if not page["has_more"]:
            break
        cursor = page["next_cursor"]
    assert seen == sorted(seen, key=int)
    assert len(seen) == len(set(seen)), "no record is delivered twice"
    everything = harness.client.get("/logs/records?limit=100").json["items"]
    assert len(seen) == len(everything), "and none is skipped"


def test_a_global_sequence_orders_both_sources(harness: Harness) -> None:
    """The contract assigns `seq` on the STM32 at capture precisely so that
    `source=all` has a stable order."""
    page = harness.client.get("/logs/records?source=all&limit=100").json
    sequence = [int(record["seq"]) for record in page["items"]]
    assert sequence == sorted(sequence)
    assert len({record["source"] for record in page["items"]}) == 2


def test_filtering_by_source_narrows_the_page(harness: Harness) -> None:
    page = harness.client.get("/logs/records?source=esp32&limit=100").json
    assert page["items"]
    assert {record["source"] for record in page["items"]} == {"esp32"}


def test_filtering_by_module_and_text_narrows_the_page(harness: Harness) -> None:
    page = harness.client.get("/logs/records?module=wifi&limit=100").json
    assert page["items"] and {r["module"] for r in page["items"]} == {"wifi"}
    page = harness.client.get("/logs/records?contains=DHCP&limit=100").json
    assert page["items"]
    assert all("dhcp" in record["message"].lower() for record in page["items"])


def test_min_level_does_not_discard_records_with_no_level(harness: Harness) -> None:
    """The contract is explicit: a level the device could not parse is not thereby
    less important than one it could, and boot and UART output have none."""
    page = harness.client.get("/logs/records?min_level=error&limit=100").json
    levels = {record["level"] for record in page["items"]}
    assert "error" in levels
    assert "info" not in levels and "debug" not in levels
    assert None in levels or "unknown" in levels


def test_a_cursor_from_a_different_filter_is_refused(harness: Harness) -> None:
    """The contract binds a cursor to its filters and tells the client to drop it
    when they change; a mock that accepted it would let the UI silently page
    through the wrong set."""
    cursor = harness.client.get("/logs/records?source=all&limit=5").json["next_cursor"]
    response = harness.client.get(f"/logs/records?source=esp32&limit=5&cursor={cursor}")
    assert response.status == 400
    assert response.json["error"]["code"] == "invalid_cursor"


def test_a_corrupt_or_empty_cursor_is_refused(harness: Harness) -> None:
    for cursor in ("not-a-real-cursor", "", "0"):
        response = harness.client.get(f"/logs/records?cursor={cursor}")
        assert response.status == 400, cursor
        assert response.json["error"]["code"] == "invalid_cursor"


def test_a_limit_outside_the_declared_range_is_an_invalid_query(harness: Harness) -> None:
    for limit in (0, 101):
        response = harness.client.get(f"/logs/records?limit={limit}")
        assert response.status == 400
        assert response.json["error"]["code"] == "invalid_query"


def test_an_unknown_query_parameter_is_refused(harness: Harness) -> None:
    """The contract rejects unknown fields in requests, and a typo that silently
    returned an unfiltered page would be worse than an error."""
    response = harness.client.get("/logs/records?levl=error")
    assert response.status == 400
    assert response.json["error"]["code"] == "invalid_query"


def test_the_awkward_records_are_present(harness: Harness) -> None:
    """A null level from early boot, a reset marker for the C6, a pause marker for
    the flashing window, and a line that was cut. Every one is a case section 7
    requires the screen to render."""
    items = harness.client.get("/logs/records?limit=100").json["items"]
    assert any(record["level"] is None for record in items)
    kinds = {record["kind"] for record in items}
    assert {"message", "reset", "paused"} <= kinds
    assert any(record["truncated"] for record in items)
    assert any("\x1b[" in record["message"] for record in items), (
        "raw ANSI, so the UI is forced to treat a log line as text and not as markup"
    )


def test_export_gives_one_record_per_line_as_ndjson(harness: Harness) -> None:
    response = harness.client.get("/logs/export?format=ndjson&max_records=10")
    assert response.status == 200
    assert response.headers["Content-Type"] == "application/x-ndjson"
    assert "attachment" in response.headers["Content-Disposition"]
    lines = response.body.decode().strip().split("\n")
    assert len(lines) == 10
    assert all(json.loads(line)["seq"] for line in lines)


def test_export_as_text_carries_a_warning_line(harness: Harness) -> None:
    """The contract asks for one: an export is a bounded snapshot and promises no
    history before the ring started."""
    response = harness.client.get("/logs/export?format=text&max_records=5")
    assert response.headers["Content-Type"].startswith("text/plain")
    lines = response.body.decode().strip().split("\n")
    assert lines[0].startswith("#")
    assert len(lines) == 6


def test_export_respects_the_declared_maximum(harness: Harness) -> None:
    for value in (0, 2001):
        assert harness.client.get(f"/logs/export?max_records={value}").status == 400
