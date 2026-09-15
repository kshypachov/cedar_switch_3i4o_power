#!/bin/zsh
# 2026-09-14 12:00: ICACHE hit/miss counters around storage_bench on the fast (09-13) and slow (09-14) interrupt images.
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning; PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
echo "=== $(date +%H:%M:%S) icache_chain: waiting for diag_chain"
while pgrep -f '^/bin/zsh /.*/diag_chain\.sh' > /dev/null || pgrep -f '[Pp]ython.* /.*/(threads_idle|run_trial|run_fabrics)\.py' > /dev/null; do sleep 10; done
sleep 5
$PY -m py_compile $T/diag/icache_monitor.py || { echo "=== icache_monitor.py does not compile"; exit 1; }
for pair in int-0913:file-spi_interrupt-y int-0914:file-spi_interrupt-y3; do
  n=${pair%%:*}; b=${pair#*:}
  [ -f $T/diag/icache-$n/icache.json ] && continue
  echo "=== $(date +%H:%M:%S) icache $n ($b)"
  $PY $T/diag/icache_monitor.py $n $S/builds/$b/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin > $S/logs/icache-$n.out 2>&1
  echo "rc=$? $(tail -1 $S/logs/icache-$n.out | cut -c1-400)"
done
echo "=== $(date +%H:%M:%S) icache diag finished"
