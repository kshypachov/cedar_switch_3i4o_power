#!/bin/zsh
# run_queue2.sh <queue file>: lines "backend|trial|kind|build-dir|change|files"; runs them in order on the board.
# A trial counts as done when tuning/<backend>/<trial>/metrics.json exists (backend-qualified,
# unlike run_queue.sh whose done list collided between zms/001-base and file/001-base).
set -u
Q=$1; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
S=${0:A:h}
while pgrep -f run_trial.py > /dev/null; do sleep 10; done   # let a trial started by the old runner finish
while IFS='|' read -r backend trial kind build change files; do
  [[ -z "$backend" || "$backend" == \#* ]] && continue
  [ -f $T/$backend/$trial/metrics.json ] && continue
  echo "=== $(date +%H:%M:%S) $backend $trial"
  /Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3 $T/run_trial.py --backend $backend --trial $trial --kind $kind \
     --image $build/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin --work $S/work --change "$change" \
     --files ${=files} > $S/logs/$backend-$trial.out 2>&1
  echo "rc=$? $(tail -1 $S/logs/$backend-$trial.out)"
done < $Q
echo "=== $(date +%H:%M:%S) queue finished"
