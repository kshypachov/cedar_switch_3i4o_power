#!/bin/zsh
# run_queue.sh <queue file>: one line per trial "backend trial kind build-dir change [files...]", runs them in order on the board.
# A line is marked done by appending to <queue>.done; rerunning skips done lines.
set -u
Q=$1; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
S=${0:A:h}
touch $Q.done
while IFS='|' read -r backend trial kind build change files; do
  [[ -z "$backend" || "$backend" == \#* ]] && continue
  grep -qxF "$trial" $Q.done && continue
  echo "=== $(date +%H:%M:%S) $backend $trial"
  /Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3 $T/run_trial.py --backend $backend --trial $trial --kind $kind \
     --image $build/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin --work $S/work --change "$change" \
     --files ${=files} > $S/logs/$backend-$trial.out 2>&1
  echo "rc=$? $(tail -1 $S/logs/$backend-$trial.out)"
  echo "$trial" >> $Q.done
done < $Q
