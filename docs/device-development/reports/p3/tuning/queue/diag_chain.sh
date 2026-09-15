#!/bin/zsh
# 2026-09-14 11:45: idle thread load + storage_bench on interrupt/base images built 09-13 19:12 vs 09-14.
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning; PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
echo "=== $(date +%H:%M:%S) diag_chain: waiting for the running trial"
while pgrep -f '[Pp]ython.* /.*/run_(trial|fabrics)\.py --backend' > /dev/null; do sleep 10; done
for pair in int-0913:file-spi_interrupt-y int-0914:file-spi_interrupt-y3 base-0913:file-base2 base-0914:file-base3; do
  n=${pair%%:*}; b=${pair#*:}
  [ -f $T/diag/$n/threads.json ] && continue
  echo "=== $(date +%H:%M:%S) diag $n ($b)"
  $PY $T/diag/threads_idle.py $n $S/builds/$b/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin > $S/logs/diag-$n.out 2>&1
  echo "rc=$? $(tail -1 $S/logs/diag-$n.out | cut -c1-300)"
done
echo "=== $(date +%H:%M:%S) diag finished"
