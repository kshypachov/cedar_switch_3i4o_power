#!/bin/zsh
# 2026-09-15, owner: flash board B (J-Link) with the latest firmware and commission it.
# Latest firmware = pka-c-on (SRAM relocation + PKA PSA driver + builtin ECP P-256 hook, p256-m), the image measured
# on board A as fabrics/zms-pka-c-on / zms-pka-c2-on. Upload through the bench MCUboot CDC recovery (fast conf, mtu 4096),
# check the boot, then the five-fabric run over the board B console (no ST-Link flash), comparable with board A.
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad
W=/Volumes/Programming/Zephyr/zephyr_latest
T=$W/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
PY=$W/.venv/bin/python3
B=$S/boardb2
C=$S/tuning/trials/crypto
IMG=$S/tuning/builds/pka-c-on/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin
echo "=== $(date +%H:%M:%S) boardb_pka_chain: upload $IMG"
P6_MCUMGR_MTU=4096 $PY $W/cedar_p6/docs/device-development/reports/p6/hw/run_image_b.py $B $IMG 90 < /dev/null > $B/upload.out 2>&1
echo "run_image_b rc=$?"
grep -E "sha256|J-Link reset|recovery|upload rc=" $B/upload.out | cut -c1-200
if ! grep -q "upload rc=0" $B/upload.out; then echo "=== upload failed"; tail -20 $B/upload.out | cut -c1-200; exit 1; fi
if ! grep -q "Matter stack initialized" $B/image_console.log; then
  echo "=== board B did not report Matter ready after the upload; last console lines:"; tail -15 $B/image_console.log | cut -c1-160; exit 1
fi
grep -E "Booting Zephyr|stm32_pka|pka|W5500|Link speed|Matter stack initialized" $B/image_console.log | head -8 | cut -c1-160
echo "=== $(date +%H:%M:%S) fabrics zms-boardb-pka-c-on"
$PY $T/fabrics/run_fabrics.py --backend zms --name boardb-pka-c-on --no-flash --console-serial 5AE6020889 \
   --image $IMG --work $S/tuning/work --change "board B, s3 + p256-m + PKA PSA driver + builtin ECP P-256 hook (pka-c-on image)" \
   --files $C/p256m.conf $C/pka.conf $C/pka-ecp.conf < /dev/null > $B/fabrics.out 2>&1
echo "rc=$? $(tail -1 $B/fabrics.out | cut -c1-300)"
echo "=== $(date +%H:%M:%S) boardb_pka_chain finished"
