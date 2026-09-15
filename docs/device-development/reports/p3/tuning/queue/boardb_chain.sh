#!/bin/zsh
# 2026-09-15, owner: flash board B with the production firmware on the upstream W5500 driver
# (prod-zms-shell-w5500up: ZMS 4 sectors, polled SPI, settings shell), commission and compare
# with board A (patched driver, fabrics/zms-prod-sectors4). Waits for the P6 run_image_b.py
# upload, checks the boot, then the five-fabric run over the board B console (no ST-Link flash).
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad
W=/Volumes/Programming/Zephyr/zephyr_latest
T=$W/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
PY=$W/.venv/bin/python3
IMG=$S/tuning/builds/prod-zms-shell-w5500up/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin
echo "=== $(date +%H:%M:%S) boardb_chain: waiting for the upload"
while pgrep -f 'run_image_b\.py' > /dev/null; do sleep 5; done
grep -E "upload rc=|recovery" $S/boardb/upload.out
if ! grep -q "upload rc=0" $S/boardb/upload.out; then echo "=== upload failed"; exit 1; fi
if ! grep -q "Matter stack initialized" $S/boardb/image_console.log; then
  echo "=== board B did not report Matter ready after the upload; last console lines:"; tail -15 $S/boardb/image_console.log | cut -c1-160; exit 1
fi
grep -E "Booting Zephyr|4 Sectors|W5500|eth_w5500|Link speed|wlan|Matter stack initialized" $S/boardb/image_console.log | head -8 | cut -c1-160
echo "=== $(date +%H:%M:%S) fabrics zms-boardb-prod-sectors4-w5500up"
$PY $T/fabrics/run_fabrics.py --backend zms --name boardb-prod-sectors4-w5500up --no-flash --console-serial 5AE6020889 \
   --image $IMG --work $S/work --change "board B, upstream W5500 driver, prod ZMS 4 sectors, polled SPI" \
   < /dev/null > $S/boardb/fabrics.out 2>&1
echo "rc=$? $(tail -1 $S/boardb/fabrics.out | cut -c1-300)"
echo "=== $(date +%H:%M:%S) boardb run finished"
