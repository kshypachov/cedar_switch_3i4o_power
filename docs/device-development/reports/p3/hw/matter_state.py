import sys, time
sys.path.insert(0, "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench")
from bench_console import Console
out = sys.argv[1]
c = Console(out + ".raw")
time.sleep(1.5)
for cmd in ["", "matter fabric count", "matter commissioning status", "kernel uptime", "net iface"]:
    text = c.collect(cmd, quiet=1.5, timeout=8)
    print(f"### {cmd}\n{text}\n", flush=True)
c.close()
