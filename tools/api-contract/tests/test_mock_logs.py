# SPDX-License-Identifier: Apache-2.0
"""Log paging, filters, and the cursor the contract makes opaque.

The ring's own hard parts are tested in `log-store`'s sim suite on the device
side; since P5 the mock reproduces what a frontend meets of them — a wrap and its
gap, a cursor from a previous boot, a page cut by the device's response buffer or
by its scan budget, an export with a gap record — because a screen built against
a mock that never produced them would show none of them correctly.
"""

from __future__ import annotations

import json

from cedar_contract.mock.constants import LOG_PAGE_BYTES
from cedar_contract.mock.wire import Request

from .conftest import Document, Harness, build


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


# -- P5: what the device's log-store does ----------------------------------


def _logged_in(document: Document, **scenario: object) -> Harness:
    harness = build(document, setup_required=False, **scenario)
    harness.client.login()
    return harness


def test_esp32_logs_follow_the_uart_owner_not_the_transport(document: Document) -> None:
    """Board B's C6 has no firmware, so ESP-Hosted never comes up — and its ROM
    output is still logged, because the console owns the UART."""
    harness = _logged_in(document, coprocessor_state="failed")
    features = harness.client.get("/capabilities").json["features"]
    assert features["esp32_logs"] == {"available": True, "reason": None}
    assert harness.client.get("/coprocessor/status").json["transport_ready"] is False

    for mode, reason in (
        ("usb_bridge", "uart_usb_bridge"),
        ("flashing", "uart_flashing"),
        ("unavailable", "uart_unavailable"),
    ):
        harness.state.scenario.uart_mode = mode
        features = harness.client.get("/capabilities").json["features"]
        assert features["esp32_logs"] == {"available": False, "reason": reason}
        esp32 = harness.client.get("/logs/sources").json["items"][1]
        assert (esp32["available"], esp32["reason"]) == (False, reason)
        assert harness.client.get("/coprocessor/status").json["uart_mode"] == mode


def test_generations_stm32_zero_and_esp32_the_coprocessors(harness: Harness) -> None:
    items = harness.client.get("/logs/sources").json["items"]
    assert items[0]["generation"] == 0
    assert items[1]["generation"] == harness.client.get("/coprocessor/status").json["generation"]
    assert items[1]["generation"] >= 1


def test_dropped_count_is_not_what_the_ring_overwrote(document: Document) -> None:
    """Overwrites are what `gap` reports; `dropped_count` counts records that never
    reached a ring, and a busy ring must not inflate it."""
    harness = _logged_in(document, log_rate_per_s=100, log_ring_records=50)
    before = harness.client.get("/logs/sources").json["items"]
    harness.advance(10)
    after = harness.client.get("/logs/sources").json["items"]
    assert [s["dropped_count"] for s in before] == [s["dropped_count"] for s in after]


def test_a_cursor_from_a_previous_boot_returns_the_tail_with_a_gap(document: Document) -> None:
    """The contract: "cursor прежнего boot → текущий хвост, новый boot_id, gap=true".
    Not a 400: the client did nothing wrong, the device restarted under it."""
    harness = _logged_in(document)
    old = harness.client.get("/logs/records?limit=5").json
    reboot = harness.app.handle(Request("POST", "/__mock/reboot", {}, b""))
    assert reboot.status == 200
    assert harness.client.get("/logs/records?limit=5").status == 401, "a reboot ends sessions"
    assert harness.client.login().status == 200, "the password survives it"

    page = harness.client.get(f"/logs/records?limit=5&cursor={old['next_cursor']}").json
    assert page["boot_id"] != old["boot_id"]
    assert page["gap"] is True
    fresh = harness.client.get("/logs/records?limit=5").json
    assert [r["seq"] for r in page["items"]] == [r["seq"] for r in fresh["items"]]
    assert fresh["gap"] is False


def test_an_empty_cursor_is_refused_and_a_forged_one_too(harness: Harness) -> None:
    good = harness.client.get("/logs/records?limit=5").json["next_cursor"]
    assert harness.client.get("/logs/records?cursor=").status == 400
    forged = harness.state.logs._encode_cursor(10_000, {})
    response = harness.client.get(f"/logs/records?cursor={forged}")
    assert response.status == 400, "a position past the newest record was never issued"
    assert response.json["error"]["code"] == "invalid_cursor"
    tampered = good[:-2] + ("AA" if good[-2:] != "AA" else "BB")
    assert harness.client.get(f"/logs/records?cursor={tampered}").status == 400


def test_a_page_stops_at_the_devices_response_buffer(document: Document) -> None:
    """The device builds a page in a 16 KiB buffer, so 100 long records do not
    fit; the page stops before the one that would not, and the cursor resumes at
    that record — nothing is skipped."""
    harness = _logged_in(document, log_rate_per_s=50)
    harness.advance(10)
    first = harness.client.get("/logs/records?limit=100")
    assert len(first.body) <= LOG_PAGE_BYTES
    page = first.json
    assert 0 < len(page["items"]) < 100
    assert page["has_more"] is True

    second = harness.client.get(f"/logs/records?limit=100&cursor={page['next_cursor']}").json
    assert int(second["items"][0]["seq"]) == int(page["items"][-1]["seq"]) + 1


def test_a_page_can_be_empty_and_still_have_more(document: Document) -> None:
    """A filter that matches nothing is bounded by records looked at, not by the
    size of the ring; the cursor carries the scan forward, and following it
    reaches the end without seeing any record twice."""
    harness = _logged_in(document, log_scan_budget=10)
    cursor = harness.state.logs._encode_cursor(0, {"module": "nothing-has-this"})
    pages = 0
    while True:
        page = harness.client.get(
            f"/logs/records?module=nothing-has-this&cursor={cursor}"
        ).json
        pages += 1
        assert page["items"] == []
        if not page["has_more"]:
            break
        assert page["next_cursor"] != cursor, "the scan position moves"
        cursor = page["next_cursor"]
    assert pages == 5, "45 records, 10 looked at per page"


def test_a_ring_that_wraps_under_a_slow_reader_reports_a_gap(document: Document) -> None:
    harness = _logged_in(document, log_rate_per_s=20, log_ring_records=30)
    tail = harness.client.get("/logs/records?limit=5").json
    assert tail["gap"] is False
    harness.advance(0.5)
    kept = harness.client.get(f"/logs/records?limit=5&cursor={tail['next_cursor']}").json
    assert kept["gap"] is False, "ten new records overwrote only records before the cursor"
    harness.advance(60)
    lost = harness.client.get(f"/logs/records?limit=5&cursor={kept['next_cursor']}").json
    assert lost["gap"] is True
    oldest = min(
        int(record["seq"])
        for record in harness.client.get("/logs/records?limit=100").json["items"]
    )
    assert int(lost["items"][0]["seq"]) >= oldest, "it continues at the oldest record kept"


def test_contains_folds_ascii_letters_only(harness: Harness) -> None:
    assert harness.client.get("/logs/records?contains=dhcp").json["items"]
    state = harness.state.logs
    state._append("stm32", "info", "test", "Ошибка ÉCLAIR", "message")
    assert harness.client.get("/logs/records?contains=%C3%89CLAIR").json["items"]
    assert harness.client.get("/logs/records?contains=%C3%A9clair").json["items"] == [], (
        "the device folds A-Z only, so É and é are different"
    )


def test_export_marks_a_part_overwritten_while_it_was_sent(document: Document) -> None:
    harness = _logged_in(document, log_export_lost=3)
    body = harness.client.get("/logs/export?format=ndjson&max_records=10").body.decode()
    lines = [json.loads(line) for line in body.strip().split("\n")]
    gap = lines[0]
    assert gap["kind"] == "gap"
    assert gap["level"] is None and gap["module"] is None
    assert gap["message"] == "records were overwritten before they could be exported"
    assert gap["seq"] == lines[1]["seq"], "the seq of the first record after the gap"
    assert gap["source"] in ("stm32", "esp32")
    assert len(lines) == 1 + 7


def test_export_text_format_is_the_devices(document: Document) -> None:
    harness = _logged_in(document, log_export_lost=2)
    harness.state.logs._append("stm32", None, None, "two\r\nlines", "message")
    response = harness.client.get("/logs/export?format=text&max_records=6")
    assert response.headers["Content-Type"] == "text/plain; charset=utf-8"
    assert response.headers["Content-Disposition"] == 'attachment; filename="cedar-logs.txt"'
    lines = response.body.decode().split("\n")
    boot_id = harness.client.get("/system/status").json["boot_id"]
    assert lines[0] == (
        f"# cedar logs {boot_id}: bounded snapshot, nothing from before this boot; gaps are marked"
    )
    assert lines[1] == "# gap: records were overwritten before they could be exported"
    assert lines[-2].endswith(" stm32 - - two\\nlines"), "CR/LF inside a message is escaped"
    ndjson = harness.client.get("/logs/export")
    assert ndjson.headers["Content-Disposition"] == 'attachment; filename="cedar-logs.ndjson"'


def test_export_text_escapes_a_lone_lf_and_a_lone_cr(document: Document) -> None:
    harness = _logged_in(document)
    harness.state.logs._append("stm32", None, None, "a\nb", "message")
    harness.state.logs._append("stm32", None, None, "c\rd", "message")
    lines = harness.client.get("/logs/export?format=text&max_records=2").body.decode().split("\n")
    assert lines[1].endswith(" stm32 - - a\\nb"), lines
    assert lines[2].endswith(" stm32 - - c\\nd"), lines


def test_a_cursor_at_the_record_the_ring_just_overwrote_reports_a_gap(harness: Harness) -> None:
    """The boundary of `gap`: the cursor resumes at exactly the record the ring
    took last. That record is lost, so the page must say so."""
    logs = harness.state.logs
    ring = logs._rings["stm32"]
    oldest = int(ring[0]["seq"])
    cursor = logs._encode_cursor(oldest, {"source": "stm32"})
    page = harness.client.get(f"/logs/records?source=stm32&limit=5&cursor={cursor}").json
    assert page["gap"] is False

    harness.state.scenario.log_ring_records = len(ring) - 1
    page = harness.client.get(f"/logs/records?source=stm32&limit=5&cursor={cursor}").json
    assert logs._overwritten["stm32"] == oldest
    assert page["gap"] is True
    assert int(page["items"][0]["seq"]) > oldest


def test_dropped_count_is_the_selected_sources(harness: Harness) -> None:
    harness.state.logs.lost["stm32"] = 2
    harness.state.logs.lost["esp32"] = 17
    for source, expected in (("stm32", "2"), ("esp32", "17"), ("all", "19")):
        page = harness.client.get(f"/logs/records?source={source}&limit=1").json
        assert page["dropped_count"] == expected, source


def _envelope_bytes(boot_id: str, dropped: str, cursor_len: int) -> int:
    return len(json.dumps(
        {"boot_id": boot_id, "items": [], "next_cursor": "x" * cursor_len,
         "has_more": False, "gap": False, "dropped_count": dropped},
        ensure_ascii=False, separators=(",", ":"),
    ).encode())


def _record_costing(harness: Harness, module: str, cost: int) -> None:
    """Append one record whose JSON is exactly `cost` bytes."""
    logs = harness.state.logs
    logs._append("stm32", "info", module, "m", "message")
    record = logs._rings["stm32"][-1]
    now = len(json.dumps(record, ensure_ascii=False, separators=(",", ":")).encode())
    record["message"] = "m" * (1 + cost - now)


def test_a_record_that_fills_the_buffer_exactly_is_kept(harness: Harness) -> None:
    """A page may be as large as the device's buffer, not one byte less."""
    from cedar_contract.mock.constants import LOG_CURSOR_RESERVE

    boot_id = harness.client.get("/system/status").json["boot_id"]
    envelope = _envelope_bytes(boot_id, "17", LOG_CURSOR_RESERVE)
    _record_costing(harness, "exactfit", LOG_PAGE_BYTES - envelope)
    response = harness.client.get("/logs/records?module=exactfit&limit=100")
    assert len(response.json["items"]) == 1
    assert len(response.body) <= LOG_PAGE_BYTES


def test_a_page_keeps_room_for_the_cursor_it_has_not_encoded_yet(harness: Harness) -> None:
    """A record that would fit only if the cursor took no room is left for the next
    page: the finished page, cursor included, must stay within the buffer."""
    boot_id = harness.client.get("/system/status").json["boot_id"]
    envelope = _envelope_bytes(boot_id, "17", 0)
    _record_costing(harness, "almostfit", LOG_PAGE_BYTES - envelope)
    response = harness.client.get("/logs/records?module=almostfit&limit=100")
    assert len(response.body) <= LOG_PAGE_BYTES
    assert response.json["items"] == []
    assert response.json["has_more"] is True
