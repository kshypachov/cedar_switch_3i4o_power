#!/bin/zsh
# After the File first pass: build file-comboA (read-size 32 + SPI interrupt) and run its combo/confirm trials.
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
echo "=== $(date +%H:%M:%S) file_finish: waiting for runner pid 10976"
while kill -0 10976 2>/dev/null; do sleep 15; done
while pgrep -f '[Pp]ython.* /.*/run_trial\.py --backend' > /dev/null; do sleep 10; done
echo "=== $(date +%H:%M:%S) building file-comboA"
$T/build_trial.sh file /private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning/builds/file-comboA /private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning/trials/file2/comboA.conf /private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning/trials/file2/comboA.overlay
if [ ! -f /private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning/builds/file-comboA/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin ] || grep -q 'NOT APPLIED' /private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning/builds/file-comboA.check; then
  echo "=== $(date +%H:%M:%S) file-comboA build FAILED"; exit 1
fi
grep -q 'read-size = < 0x20 >' /private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning/builds/file-comboA.check || echo "=== WARNING: read-size 32 not seen in zephyr.dts check"
$S/run_queue3.sh $S/queue6.txt
