"""Board B only: where is a hung STM32? Halt through J-Link without a reset, read the core
registers and the stack, resolve PC, LR and code addresses on the stack in the image's ELF,
then let the core run again.

usage: jlink_dump_b.py <zephyr.elf> <out file>

Reading registers does not disturb the C6 or the flash. The core is resumed ("g") at the end,
so a hang that is a busy loop keeps its state for a second look.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board_b as b  # noqa: E402

ELF, OUT = sys.argv[1], sys.argv[2]
ADDR2LINE = shutil.which("arm-zephyr-eabi-addr2line") or str(
    next(Path.home().glob("zephyr-sdk-*/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-addr2line")))


def resolve(addresses):
    if not addresses:
        return {}
    r = subprocess.run([ADDR2LINE, "-f", "-C", "-e", ELF] + [hex(a & ~1) for a in addresses],
                       capture_output=True, text=True)
    lines = r.stdout.splitlines()
    return {a: f"{lines[2 * i]} {lines[2 * i + 1]}" for i, a in enumerate(addresses) if 2 * i + 1 < len(lines)}


first = b.jlink("h\nregs\n")
regs = dict(re.findall(r"\b(R\d+|SP\(R13\)|MSP|PSP|LR|PC)\s*=\s*([0-9A-Fa-f]{8})", first.stdout))
pc = int(regs.get("PC", "0"), 16)
lr = int(regs.get("LR", "0"), 16)
sp_text = regs.get("SP(R13)") or regs.get("PSP") or regs.get("MSP") or "0"
sp = int(sp_text, 16)
second = b.jlink(f"h\nmem32 {sp:08X} 64\ng\n") if sp else None
words = []
for row in re.findall(r"=\s*((?:[0-9A-Fa-f]{8}\s*)+)", second.stdout if second else ""):
    words.extend(int(word, 16) for word in row.split())
code = sorted({w for w in words if 0x08000000 <= w < 0x08400000 or 0x90000000 <= w < 0x90400000
               or 0x02000000 <= w < 0x02400000})
names = resolve([pc, lr] + code[:40])
with open(OUT, "w") as f:
    f.write("# J-Link halt without reset, board B\n## regs\n" + first.stdout[-3000:] + "\n")
    f.write(f"## PC {pc:#010x}: {names.get(pc)}\n## LR {lr:#010x}: {names.get(lr)}\n")
    f.write(f"## stack at {sp:#010x}\n" + (second.stdout[-4000:] if second else "") + "\n## code addresses on the stack\n")
    for a in code[:40]:
        f.write(f"{a:#010x} {names.get(a)}\n")
print(open(OUT).read()[-2500:])
