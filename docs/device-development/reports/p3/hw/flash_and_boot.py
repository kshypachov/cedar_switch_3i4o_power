"""Flash the application image and capture the console through the reset.

usage: flash_and_boot.py <signed.bin> <out-prefix> [seconds]
Writes <out-prefix>.raw (console) and <out-prefix>.out (flash log + Matter lines).
"""
import subprocess, sys, time
sys.path.insert(0, "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench")
from bench_console import Console

CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
EL = "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"
SN = "002F002B3233510739363634"

image, prefix = sys.argv[1], sys.argv[2]
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 60
out = open(prefix + ".out", "w")
def log(text):
    print(text, flush=True); out.write(text + "\n"); out.flush()

console = Console(prefix + ".raw")
time.sleep(1.0)
t0 = time.time()
w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", EL, "-w", image, "0x90000000", "-v"], capture_output=True, text=True)
log(f"flash rc={w.returncode} in {time.time()-t0:.1f}s")
log("\n".join(l for l in w.stdout.splitlines() if "Error" in l or "verified" in l.lower() or "Download" in l)[-800:])
if w.returncode != 0:
    sys.exit(1)
mark = console.mark()
r = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
log(f"reset rc={r.returncode} at {time.strftime('%H:%M:%S')}")
time.sleep(seconds)
keys = ("chip", "matter", "DIS", "dns", "mdns", "err", "wrn", "fault", "web_server", "Starting bootloader")
for when, text in console.lines_since(mark):
    if any(k.lower() in text.lower() for k in keys):
        log(f"+{when - t0:7.2f}s {text[:220]}")
console.close()
