"""Board B only: put an image into slot0 through MCUboot CDC recovery and capture its console.
usage: run_test_image_b.py <workdir> <signed.bin> <listen seconds>"""
import subprocess, sys, threading, time
sys.path.insert(0, "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench")
from bench_console import resolve_port
import serial
P, IMG, LISTEN = sys.argv[1], sys.argv[2], float(sys.argv[3])
UART = "/dev/cu.usbmodem5AE60208891"; SN_B = "3543501200210047"
JLINK = ["JLinkExe", "-USB", "000941000024", "-device", "STM32U585AI", "-if", "SWD",
         "-speed", "4000", "-autoconnect", "1", "-NoGui", "1", "-ExitOnError", "0"]
def jlink(cmds):
    open(f"{P}/_tmp.jlink", "w").write(cmds + "q\n")
    subprocess.run(JLINK + ["-CommanderScript", f"{P}/_tmp.jlink"], capture_output=True)
log = bytearray(); lock = threading.Lock(); stop = threading.Event()
def reader():
    s = serial.Serial(); s.port = UART; s.baudrate = 115200; s.timeout = 0.1
    s.dtr = False; s.rts = False; s.open()
    while not stop.is_set():
        d = s.read(4096)
        if d:
            with lock: log.extend(d)
    s.close()
threading.Thread(target=reader, daemon=True).start(); time.sleep(0.3)
def mcumgr(port, *a, t=1, r=1):
    return subprocess.run(["/Users/kiro/go/bin/mcumgr", "--conntype", "serial", "--connstring",
                           f"dev={port},baud=115200,mtu=512", "-t", str(t), "-r", str(r), *a],
                          capture_output=True, text=True)
jlink("r\ng\n"); t0 = time.time(); port = None
while time.time() - t0 < 12:
    try:
        port = resolve_port(SN_B)
        c = mcumgr(port, "image", "list")
        if c.returncode == 0 and "Images:" in c.stdout: break
    except LookupError:
        time.sleep(0.1)
    port = None
if not port: stop.set(); sys.exit("recovery did not answer")
print(f"recovery on {port} after {time.time()-t0:.1f} s", flush=True)
t1 = time.time(); up = mcumgr(port, "image", "upload", IMG, t=10, r=3)
print(f"upload rc={up.returncode} in {time.time()-t1:.0f} s", flush=True)
print(mcumgr(port, "image", "list", t=5).stdout, flush=True)
with lock: log.clear()
jlink("r\ng\n"); time.sleep(LISTEN); stop.set(); time.sleep(0.3)
text = bytes(log).decode("utf-8", "replace").replace("\r", "")
open(f"{P}/test_image_console.log", "w").write(text)
print(text[-6000:])
