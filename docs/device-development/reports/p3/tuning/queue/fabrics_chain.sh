#!/bin/zsh
# 2026-09-14 09:50, after file/032-comboA (read-size 32 + interrupt) came out worse than the base:
# let the running 033 repeat finish, build while the board is idle, run File comboB (interrupt +
# no sleep while waiting) and an interrupt-only repeat, the 10-cycle confirmation of the better of
# the two, then the five-fabric check (ZMS 32 -> 8 -> 4 sectors, File interrupt MAX_LINES 128 -> 96 -> 64).
# Five-fabric queue lines "backend|name|build|change|files"; done when fabrics/<backend>-<name>/metrics.json exists.
set -u
S=${0:A:h}; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
echo "=== $(date +%H:%M:%S) fabrics_chain: waiting for the running trial"
while pgrep -f '[Pp]ython.* /.*/run_(trial|fabrics)\.py --backend' > /dev/null; do sleep 10; done
echo "=== $(date +%H:%M:%S) building file-comboB and five-fabric candidates"
$S/build_batch.sh $S/build-fabrics.list 3
for n in file-comboB zms-comboA-sectors8 file-spi_interrupt-ml96 file-spi_interrupt-ml64; do
  if [ ! -f $S/builds/$n/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin ] || grep -q 'NOT APPLIED' $S/builds/$n.check; then
    echo "=== $(date +%H:%M:%S) $n build FAILED"; exit 1
  fi
done
$S/run_queue3.sh $S/queue7.txt
# 10-cycle confirmation of the better File combination: comboB only if it beats the
# interrupt-only runs (027, 036) by more than the File noise (0.60 s).
best=$($PY - <<EOF
import json, statistics
from pathlib import Path
T = Path("$T/file")
s = lambda t: json.loads((T / t / "metrics.json").read_text())["score"] if (T / t / "metrics.json").exists() else None
b = s("035-comboB"); i = [x for x in (s("027-spi_interrupt-y"), s("036-spi_interrupt-confirm5")) if x is not None]
print("comboB" if b is not None and i and b < statistics.median(i) - 0.60 else "spi_interrupt")
EOF
)
echo "=== $(date +%H:%M:%S) File 10-cycle confirmation: $best"
if [ "$best" = comboB ]; then
  echo "file|037-comboB-confirm10|confirm|$S/builds/file-comboB|CONFIG_SPI_STM32_INTERRUPT=y CONFIG_SPI_NOR_SLEEP_WHILE_WAITING_UNTIL_READY=n|$S/trials/file3/comboB.conf|10" > $S/queue8.txt
else
  echo "file|037-spi_interrupt-confirm10|confirm|$S/builds/file-spi_interrupt-y|CONFIG_SPI_STM32_INTERRUPT=y|$S/trials/shared/spi_interrupt-y.conf|10" > $S/queue8.txt
fi
$S/run_queue3.sh $S/queue8.txt
while IFS="|" read -r backend name build change files; do
  [[ -z "$backend" || "$backend" == \#* ]] && continue
  [ -f $T/fabrics/$backend-$name/metrics.json ] && continue
  echo "=== $(date +%H:%M:%S) fabrics $backend-$name"
  $PY $T/fabrics/run_fabrics.py --backend $backend --name $name \
     --image $build/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin --work $S/work --change "$change" \
     --files ${=files} > $S/logs/fabrics-$backend-$name.out 2>&1
  echo "rc=$? $(tail -1 $S/logs/fabrics-$backend-$name.out)"
done < $S/queue-fabrics.txt
echo "=== $(date +%H:%M:%S) fabrics queue finished"
