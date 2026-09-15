"""Board B only: one shell command over telnet, printed with its answer.

usage: shell_b.py "<command>" "<regex the answer contains>" [timeout seconds]

The serial console loses input bytes (reports/p5), so bench commands go over telnet.
"""
import sys

sys.argv, ARGS = sys.argv[:1] + ["check", "/dev/null"], sys.argv[1:]
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b as b  # noqa: E402
from loader_b import Shell  # noqa: E402

command, expect = ARGS[0], ARGS[1]
timeout = float(ARGS[2]) if len(ARGS) > 2 else 15.0
b.refuse(b.HOST)
sh = Shell()
for line in sh.run(command, expect, timeout=timeout):
    print(line)
sh.close()
