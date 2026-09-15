#!/bin/zsh
# 2026-09-15, owner: STM32U585 PKA as a PSA driver for Matter, stage B on board A.
# 1) pka-b-on image: KAT, Matter crypto_bench through PSA with the PKA on and off, stats.
# 2) five-fabric commissioning: pka-b-off (software: p256-m + builtin SPAKE2+) then pka-b-on (PKA).
# Both images: s3 (hot data/code/malloc arena in SRAM) + p256-m + the PKA module; they differ only
# in CONFIG_PSA_CRYPTO_DRIVER_STM32_PKA_DEFAULT_ON.
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning
R=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports
PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
A=cedar_switch_3in4out_power/zephyr
C=$S/trials/crypto
steps=${1:-psa,fabrics}
if [[ $steps == *psa* ]]; then
  echo "=== $(date +%H:%M:%S) pka-b psa check"
  $PY $R/crypto/pka/pka_shell_run.py pka-b-psa $S/builds/pka-b-on/$A/zephyr.signed.bin \
     "pka kat" "pka stats reset" "matter crypto_bench 10" "pka stats" "pka psa off" "matter crypto_bench 10" \
     "pka psa on" "pka stats" < /dev/null > $S/logs/pka-b-psa.out 2>&1
  echo "rc=$? pka-b-psa"
fi
if [[ $steps == *fabrics* ]]; then
  for b in pka-b-off pka-b-on; do
    echo "=== $(date +%H:%M:%S) fabrics zms-$b"
    files=(--files $C/p256m.conf $C/pka.conf)
    [[ $b == *off ]] && files+=($C/pka-off.conf)
    $PY $R/p3/tuning/fabrics/run_fabrics.py --backend zms --name $b --image $S/builds/$b/$A/zephyr.signed.bin \
       --work $S/work --change "s3 + p256-m + STM32 PKA PSA driver, default ${b##*-}" $files \
       < /dev/null > $S/logs/fabrics-zms-$b.out 2>&1
    echo "rc=$? $(tail -1 $S/logs/fabrics-zms-$b.out | cut -c1-300)"
  done
fi
echo "=== $(date +%H:%M:%S) pka chain finished"
