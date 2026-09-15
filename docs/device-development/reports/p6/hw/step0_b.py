"""Board B only: step 0 of P6 - the application with the esp-loader node, before any
updater code runs. Does the library's POST_KERNEL init (USART3 callback, EN/BOOT driven
inactive) leave the P5 behaviour intact?

Checks, all read-only for the C6 except one reset through EN (allowed, owner P5):
  - HTTP up; /coprocessor/status, /logs/sources, /capabilities match their schemas
  - the ESP32 ring receives the C6's ROM output (uart_mode console, records arriving)
  - `coproc status` answers on the console
  - `wifi_ctrl reset` goes through coprocessor-manager: generation +1, a reset marker,
    ROM output again afterwards

usage: step0_b.py <out prefix>   (P6_IMAGE_SHA256 recorded when set)
"""
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b as b  # noqa: E402

PREFIX = sys.argv[1]
rec = b.Record(PREFIX + ".json")
con = b.CONSOLE_READER
dev = b.Device()


def esp32_page(query):
    s, _, body, ms = dev.call("GET", "/logs/records?source=esp32&limit=100" + query)
    return s, body, ms


def main():
    con.start()
    up = dev.wait_up(90)
    rec.step("http up", seconds=up)
    if up is None:
        return
    rec.step("login", ms=dev.login())

    for name, path in (("CoprocessorStatus", "/coprocessor/status"), ("LogSources", "/logs/sources"),
                       ("Capabilities", "/capabilities")):
        s, _, body, ms = dev.call("GET", path)
        rec.step(path, status=s, ms=ms, schema=b.schema_errors(name, body), body=body)

    s, body, ms = esp32_page("")
    first_cursor = body.get("next_cursor") if s == 200 else None
    rec.step("esp32 page", status=s, ms=ms, items=len(body.get("items", [])) if s == 200 else None,
             last=(body.get("items") or [None])[-1] if s == 200 else None)
    time.sleep(5)
    s, body, ms = esp32_page(f"&cursor={first_cursor}")
    rec.step("esp32 after 5 s", status=s, items=len(body.get("items", [])) if s == 200 else None,
             gap=body.get("gap") if s == 200 else None)

    line = con.shell("coproc status", r"uart_mode", timeout=5)
    rec.step("coproc status", line=line)

    before = dev.call("GET", "/coprocessor/status")[2]
    line = con.shell("wifi_ctrl reset", r"reset|Reset|RESET|busy|error", timeout=8)
    rec.step("wifi_ctrl reset", line=line)
    time.sleep(4)
    after = dev.call("GET", "/coprocessor/status")[2]
    rec.step("generation", before=before.get("generation"), after=after.get("generation"),
             uart_mode=after.get("uart_mode"))

    # The records query has no kind filter: page the ESP32 ring from the cursor taken
    # before the reset and keep what is not a log line (the ROM flood is most of it).
    markers = []
    cursor = first_cursor
    for _ in range(80):
        s, body, _ = esp32_page(f"&cursor={cursor}")
        if s != 200:
            rec.step("marker page", status=s, body=body)
            break
        markers.extend(i for i in body["items"] if i.get("kind") != "log")
        cursor = body["next_cursor"]
        if not body["has_more"]:
            break
    rec.step("markers since start", markers=markers[-5:])
    s, body, _ = esp32_page("")
    rec.step("esp32 after reset", status=s, last=(body.get("items") or [None])[-1] if s == 200 else None)


try:
    main()
finally:
    rec.save()
    con.save(PREFIX + ".console.log")
    con.stop()
