"""Flash an image on board A and run shell commands, recording their output.

usage: pka_shell_run.py <name> <zephyr.signed.bin|-> <command> [<command> ...]

"-" as the image skips flashing and resets the board instead. Each command is sent
with the echo checked (the console drops input bytes now and then) and its output is
collected until the end marker of that command:
  pka kat        -> "pka kat: PASS|FAIL"
  pka bench ...  -> "pka bench n="
  pka stats      -> "pka stat verify"
  matter crypto_bench N -> "P-256 ECDSA verify total="
  other          -> 5 s of output

Results in reports/crypto/pka/<name>/: console.log.gz, run.json (lines per command).
"""
import gzip, json, re, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[4]
sys.path.insert(0, str(REPO / "tests/bench"))
from bench_console import Console  # noqa: E402

CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
EL = REPO / "CLIVEONE-W25Q128_STM32U545-PB10-PA4-PB1-PB0-PA7-PA6.stldr"
SN = "002F002B3233510739363634"
END = [(r"^pka kat", r"pka kat: (PASS|FAIL)", 120), (r"^pka bench", r"pka bench n=", 600),
       # 'pka ecp check N' takes ~0.6 s per round; without its own marker the 5 s default cut it off
       # and shifted the output of every later command (2026-09-15 02:54, pka-c-ecp first run).
       (r"^pka ecp check", r"pka ecp check: (PASS|FAIL)|pka ecp check: mbedtls error", 600),
       (r"^pka (psa|ecp)\b", r"pka (psa|ecp): (on|off)", 10),
       (r"^pka stats", r"pka stat (verify|muladd)", 20), (r"^matter crypto_bench", r"P-256 ECDSA verify\s+total=", 1800)]

name, image, commands = sys.argv[1], sys.argv[2], sys.argv[3:]
OUT = HERE / name
OUT.mkdir(parents=True, exist_ok=True)
console = Console(str(OUT / "console.raw"))
time.sleep(1.0)
result = dict(name=name, image=image, started=time.strftime("%Y-%m-%d %H:%M:%S"), commands=[])

if image != "-":
    w = subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-el", str(EL), "-w", image, "0x90000000", "-v"],
                       capture_output=True, text=True)
    result["flash_ok"] = w.returncode == 0
    if w.returncode != 0:
        result["flash_tail"] = w.stdout[-800:]
mark = console.mark()
subprocess.run([CLI, "-c", "port=swd", f"sn={SN}", "mode=NORMAL", "-hardRst"], capture_output=True, text=True)
result["matter_ready"] = console.wait_for(r"Matter stack initialized|Matter Server Init failed|Matter init failed", mark, 600) is not None
boot = [t.strip()[-200:] for _, t in console.lines_since(mark) if re.search(r"stm32_pka|PKA|<err>", t)]
result["boot_lines"] = boot[:40]
time.sleep(15)

for cmd in commands:
    entry = dict(command=cmd, sent=False, finished=False, lines=[])
    sent = None
    for _ in range(4):
        m = console.mark()
        console.send(cmd)
        if console.wait_for(re.escape(cmd) + r"\s*$", m, 4) is not None:
            sent = m
            break
        time.sleep(1.0)
    if sent is not None:
        entry["sent"] = True
        end, timeout = None, 5
        for pat, e, t in END:
            if re.search(pat, cmd):
                end, timeout = e, t
                break  # first match: "pka ecp check" must not fall to the "pka ecp on|off" entry
        if end is None:
            time.sleep(timeout)
            entry["finished"] = True
        else:
            entry["finished"] = console.wait_for(end, sent, timeout) is not None
        time.sleep(0.5)
        entry["lines"] = [re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", t).strip() for _, t in console.lines_since(sent)
                          if re.search(r"pka|PASS|FAIL|total=|<err>|<wrn>|stm32_pka", t)]
    result["commands"].append(entry)

console.close()
raw = OUT / "console.raw"
with open(raw, "rb") as src, gzip.open(OUT / "console.log.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)
raw.unlink()
(OUT / "run.json").write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
print(json.dumps(result, indent=1, ensure_ascii=False))
