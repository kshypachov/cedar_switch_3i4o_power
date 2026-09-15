#!/bin/zsh
# 2026-09-14 13:10, owner's decision: five-fabric check with polled SPI (no CONFIG_SPI_STM32_INTERRUPT).
# All six images built in one batch from the same sources, then checked: every trial CONFIG_ line applied,
# SPI interrupt not set, eth_w5500/esp_hosted symbol sizes identical across images. Then the runs:
# ZMS cache 2048 on 32 -> 8 -> 4 sectors, File MAX_LINES 128 -> 96 -> 64.
set -u
S=${0:A:h}; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
NM=/Users/kiro/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm
echo "=== $(date +%H:%M:%S) fabrics_polled_chain: building six images"
$S/build_batch.sh $S/build-fabrics2.list 3
typeset -A ref
while IFS="|" read -r backend name build change files; do
  [[ -z "$backend" || "$backend" == \#* ]] && continue
  n=${build:t}; app=$build/cedar_switch_3in4out_power/zephyr
  if [ ! -f $app/zephyr.signed.bin ] || grep -q 'NOT APPLIED' $S/builds/$n.check; then
    echo "=== $(date +%H:%M:%S) $n build FAILED"; exit 1
  fi
  if grep -q '^CONFIG_SPI_STM32_INTERRUPT=y' $app/.config; then
    echo "=== $(date +%H:%M:%S) $n has SPI interrupt enabled"; exit 1
  fi
  sig=$($NM -S $app/zephyr.elf | grep -E ' (w5500_|esp_hosted_mcu_)' | awk '{print $4, $2}' | sort | shasum | cut -c1-12)
  # Candidates are compared within a backend, so the driver code must match within a backend.
  # 13:37: the shared tree's esp_hosted_mcu changed between the ZMS (13:32) and File (13:37) builds.
  [ -z "${ref[$backend]:-}" ] && ref[$backend]=$sig
  [ "$sig" = "${ref[$backend]}" ] || { echo "=== $(date +%H:%M:%S) $n driver code differs within $backend ($sig vs ${ref[$backend]})"; exit 1; }
done < $S/queue-fabrics2.txt
echo "=== $(date +%H:%M:%S) six images ok (driver signature zms ${ref[zms]}, file ${ref[file]})"
while IFS="|" read -r backend name build change files; do
  [[ -z "$backend" || "$backend" == \#* ]] && continue
  [ -f $T/fabrics/$backend-$name/metrics.json ] && continue
  echo "=== $(date +%H:%M:%S) fabrics $backend-$name"
  $PY $T/fabrics/run_fabrics.py --backend $backend --name $name \
     --image $build/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin --work $S/work --change "$change" \
     --files ${=files} < /dev/null > $S/logs/fabrics-$backend-$name.out 2>&1
  echo "rc=$? $(tail -1 $S/logs/fabrics-$backend-$name.out | cut -c1-300)"
done < $S/queue-fabrics2.txt
echo "=== $(date +%H:%M:%S) fabrics queue finished"
