import sys, time
sys.path.insert(0, "/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench")
from bench_console import Console
out = sys.argv[1]
c = Console(out + ".raw")
time.sleep(1.5)
with open(out, "w") as f:
    for cmd in sys.argv[2:]:
        text = c.collect(cmd, quiet=2.0, timeout=30) or ""
        lines = [l for l in text.splitlines() if "chip]" not in l and "<dbg>" not in l and "<inf> chip" not in l]
        f.write(f"### {cmd}\n" + "\n".join(lines) + "\n"); print(f"### {cmd}\n" + "\n".join(lines[-6:]), flush=True)
c.close()
