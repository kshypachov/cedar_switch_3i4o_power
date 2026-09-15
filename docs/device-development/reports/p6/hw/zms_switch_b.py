"""Board B only: after the settings backend moved to ZMS, erase the ZMS settings partition and
check the next boot.

Owner 2026-09-14: the test board's LittleFS and ZMS partitions may be erased, no data on it
matters; apply the fastest settings backend of the P3 measurements (ZMS, 4 sectors, lookup cache
2048, SPI interrupt). The settings written by the File backend (/lfs/settings) are not read by
ZMS, so the board boots with default settings: the network may come up differently (Ethernet
192.168.88.13, Wi-Fi 192.168.88.23 before). Board A's address is refused by board_b either way.

Steps: reset via J-Link, wait for the shell, `storage_wipe yes` on the serial console (the
console loses input bytes now and then: board_b.Console.shell retries), reset again, record the
boot, then try HTTP on the two known board B addresses.

usage: zms_switch_b.py <out prefix>
"""
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b as b  # noqa: E402

PREFIX = sys.argv[1]
ADDRESSES = ("192.168.88.13", "192.168.88.23")
rec = b.Record(PREFIX + ".json")
con = b.CONSOLE_READER


def boot(label, listen):
    con.mark(label)
    t = b.now()
    r = b.jlink("r\ng\n")
    rec.step(label, jlink_rc=r.returncode)
    time.sleep(listen)
    return con.since(t)


def main():
    for ip in ADDRESSES:
        b.refuse(ip)
    con.start()
    lines = boot("reset before wipe", 25)
    rec.step("boot before wipe", zms=[l for l in lines if "zms" in l.lower()][:6],
             storage_errors=[l for l in lines if "<err>" in l and ("fs" in l or "settings" in l
                                                                  or "zms" in l.lower())][:8])
    con.mark("storage_wipe yes")
    answer = con.shell("storage_wipe yes", r"storage_wipe: offset", timeout=60, retries=2)
    rec.step("storage_wipe", answer=answer)
    if answer is None or "rc=0" not in answer:
        return
    lines = boot("reset after wipe", 45)
    rec.step("boot after wipe",
             start=[l for l in lines if "Start main app" in l][:1],
             zms=[l for l in lines if "zms" in l.lower()][:6],
             settings=[l for l in lines if "settings" in l.lower()][:6],
             storage_errors=[l for l in lines if "<err>" in l and ("fs" in l or "settings" in l
                                                                  or "zms" in l.lower())][:8],
             faults=[l for l in lines if "FAULT" in l or "overflow" in l][:4])
    for ip in ADDRESSES:
        b.HOST = ip
        up = b.Device().wait_up(60)
        rec.step("http", host=ip, seconds=up)


try:
    main()
finally:
    rec.save()
    con.save(PREFIX + ".console.log")
    con.stop()
