"""Commission the board with chip-tool while capturing its console.

usage: commission_chip_tool.py <out-prefix> <storage-dir> <node-id> <passcode> <discriminator>
"""
import subprocess, sys, time
sys.path.insert(0, "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench")
from bench_console import Console

MATTER = "/Volumes/Programming/Zephyr/zephyr_latest/modules/lib/matter"
prefix, storage, node, passcode, disc = sys.argv[1:6]
commissioner = sys.argv[6] if len(sys.argv) > 6 else "alpha"
console = Console(prefix + ".console.raw")
time.sleep(1.0)
mark = console.mark()
t0 = time.time()
cmd = [f"{MATTER}/out/chip-tool/chip-tool", "pairing", "onnetwork-long", node, passcode, disc,
       "--storage-directory", storage,
       "--paa-trust-store-path", f"{MATTER}/credentials/development/paa-root-certs",
       "--commissioner-name", commissioner]
with open(prefix + ".chip-tool.log", "w") as log:
    rc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=240).returncode
elapsed = time.time() - t0
time.sleep(3)
with open(prefix + ".out", "w") as out:
    out.write(f"chip-tool rc={rc} elapsed={elapsed:.1f}s\n")
    keys = ("chip:", "err", "wrn", "Commissioning", "fabric", "Long dispatch", "PASE", "CASE", "fail")
    for when, text in console.lines_since(mark):
        if any(k.lower() in text.lower() for k in keys):
            out.write(f"+{when - t0:7.2f}s {text[:200]}\n")
console.close()
print(open(prefix + ".out").read()[:6000])
