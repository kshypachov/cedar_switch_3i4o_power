#!/bin/zsh
# 2026-09-14 12:12, owner's request: storage_bench with the network interfaces up/down on the slow (09-14) and fast (09-13) interrupt images.
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning; PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
echo "=== $(date +%H:%M:%S) net_chain: waiting for icache_chain"
sleep 20
while pgrep -f '^/bin/zsh /.*/(diag_chain|icache_chain)\.sh' > /dev/null || pgrep -f '[Pp]ython.* /.*/(threads_idle|icache_monitor|run_trial|run_fabrics)\.py' > /dev/null; do sleep 10; done
sleep 5
$PY -m py_compile $T/diag/net_off.py || { echo "=== net_off.py does not compile"; exit 1; }
for pair in int-0914:file-spi_interrupt-y3 int-0913:file-spi_interrupt-y; do
  n=${pair%%:*}; b=${pair#*:}
  [ -f $T/diag/netoff-$n/netoff.json ] && continue
  echo "=== $(date +%H:%M:%S) netoff $n ($b)"
  $PY $T/diag/net_off.py $n $S/builds/$b/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin > $S/logs/netoff-$n.out 2>&1
  echo "rc=$? $(tail -1 $S/logs/netoff-$n.out | cut -c1-500)"
done
echo "=== $(date +%H:%M:%S) net diag finished"
