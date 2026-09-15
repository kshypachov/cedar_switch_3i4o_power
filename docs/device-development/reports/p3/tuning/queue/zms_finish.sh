#!/bin/zsh
# Finish ZMS follow-ups (034, 035), build and run comboB (036), then continue with File.
S=${0:A:h}; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
P=$(pgrep -f '[Pp]ython.* /.*/run_trial\.py --backend' | head -1)
echo "=== $(date +%H:%M:%S) zms_finish: waiting for trial pid ${P:-none}"
while [ -n "$P" ] && kill -0 $P 2>/dev/null; do sleep 10; done
$S/run_queue2.sh $S/queue3.txt
echo "=== $(date +%H:%M:%S) building zms-comboB"
$T/build_trial.sh zms $S/builds/zms-comboB $S/trials/zms2/comboB.conf ""
Q=$S/queue4.txt
if [ ! -f $S/builds/zms-comboB/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin ] || grep -q 'NOT APPLIED' $S/builds/zms-comboB.check; then
  echo "=== $(date +%H:%M:%S) comboB build FAILED, continuing without it"
  grep -v comboB $S/queue4.txt > $S/queue4b.txt; Q=$S/queue4b.txt
fi
$S/run_queue2.sh $Q
