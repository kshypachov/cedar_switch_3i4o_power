#!/bin/zsh
# 2026-09-14 16:35: verification of the firmware switched to ZMS (prj.conf + storage_zms rename).
# 1) boot of the image as it ships; 2) five-fabric run on the same config plus the settings shell.
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning; PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
while pgrep -f '[Pp]ython.* /.*/(run_trial|run_fabrics|threads_idle|icache_monitor|net_off)\.py' > /dev/null; do sleep 10; done
echo "=== $(date +%H:%M:%S) prod boot check (prod-zms, as shipped)"
$PY $T/fabrics/prod_boot_check.py prod-boot $S/builds/prod-zms/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin < /dev/null > $S/logs/prod-boot.out 2>&1
echo "rc=$? $(grep -c '' $S/logs/prod-boot.out) lines"
echo "=== $(date +%H:%M:%S) fabrics zms-prod-sectors4 (prod-zms-shell)"
$PY $T/fabrics/run_fabrics.py --backend zms --name prod-sectors4    --image $S/builds/prod-zms-shell/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin --work $S/work    --change "prj.conf ZMS 4 sectors, cache 2048, polled SPI; storage_zms partition" < /dev/null > $S/logs/fabrics-zms-prod-sectors4.out 2>&1
echo "rc=$? $(tail -1 $S/logs/fabrics-zms-prod-sectors4.out | cut -c1-300)"
echo "=== $(date +%H:%M:%S) prod verification finished"
