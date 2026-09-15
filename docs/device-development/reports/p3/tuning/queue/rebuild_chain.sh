#!/bin/zsh
# 2026-09-14 10:45. Images built after 09-13 23:55 contain a changed zephyr/drivers/ethernet/eth_w5500.c
# (MMB: IPv4 multicast dropped in hardware; RX resync), made by another thread in the shared tree.
# Bases and single-setting trials ran on the old driver, combos on the new one. Control: rebuild the
# bases and interrupt-only on today's sources, run them as trials, then the five-fabric check with
# every image of a backend built from the same sources (all after 23:55).
set -u
S=${0:A:h}; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
echo "=== $(date +%H:%M:%S) rebuild_chain: waiting for the running trial"
while pgrep -f '[Pp]ython.* /.*/run_(trial|fabrics)\.py --backend' > /dev/null; do sleep 10; done
echo "=== $(date +%H:%M:%S) rebuilding bases and interrupt-only on current sources"
$S/build_batch.sh $S/build-rebuild.list 3
for n in file-base3 zms-base3 file-spi_interrupt-y3; do
  if [ ! -f $S/builds/$n/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin ] || grep -q 'NOT APPLIED' $S/builds/$n.check 2>/dev/null; then
    echo "=== $(date +%H:%M:%S) $n build FAILED"; exit 1
  fi
  [ "$($PY - <<EOF
import subprocess
out = subprocess.run(["/Users/kiro/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm", "$S/builds/$n/cedar_switch_3in4out_power/zephyr/zephyr.elf"], capture_output=True, text=True).stdout
print("yes" if " w5500_rx_resync" in out else "no")
EOF
)" = yes ] || { echo "=== $(date +%H:%M:%S) $n has no w5500_rx_resync, sources changed again"; exit 1; }
done
$S/run_queue3.sh $S/queue9.txt
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
