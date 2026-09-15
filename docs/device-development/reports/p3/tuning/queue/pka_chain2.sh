#!/bin/zsh
# 2026-09-15, owner: STM32U585 PKA, stage 2 — builtin ECP P-256 (Matter SPAKE2+) on the PKA
# (patches/tf-psa-crypto/ecp-p256-stm32-pka.patch). Board A.
# 1) pka-c-on: KAT (incl. muladd), 'pka ecp check 20' (mbedtls_ecp_mul/muladd PKA vs software), bench.
# 2) five-fabric commissioning: pka-c-off (PSA driver on, ECP hook off) then pka-c-on (both on).
# Images differ only in CONFIG_STM32_PKA_ECP_P256_DEFAULT_ON.
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning
R=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports
PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
A=cedar_switch_3in4out_power/zephyr
C=$S/trials/crypto
steps=${1:-check,fabrics}
if [[ $steps == *check* ]]; then
  echo "=== $(date +%H:%M:%S) pka-c ecp check"
  $PY $R/crypto/pka/pka_shell_run.py pka-c-ecp $S/builds/pka-c-on/$A/zephyr.signed.bin \
     "pka kat" "pka ecp check 20" "pka bench 10" "pka stats" < /dev/null > $S/logs/pka-c-ecp.out 2>&1
  echo "rc=$? pka-c-ecp $(grep -o 'pka ecp check: [A-Z]*\|pka kat: [A-Z]*' $R/crypto/pka/pka-c-ecp/run.json | tr '\n' ' ')"
  if ! grep -q 'pka ecp check: PASS' $R/crypto/pka/pka-c-ecp/run.json || ! grep -q 'pka kat: PASS' $R/crypto/pka/pka-c-ecp/run.json; then
    echo "=== check failed, no commissioning with the ECP hook"; exit 1
  fi
fi
if [[ $steps == *fabrics* ]]; then
  for b in pka-c-off pka-c-on; do
    name=${b/pka-c/pka-c${SERIES:-}}   # SERIES=2 -> pka-c2-off / pka-c2-on (repeat pair, same images)
    echo "=== $(date +%H:%M:%S) fabrics zms-$name"
    files=(--files $C/p256m.conf $C/pka.conf $C/pka-ecp.conf)
    [[ $b == *off ]] && files+=($C/pka-ecp-off.conf)
    $PY $R/p3/tuning/fabrics/run_fabrics.py --backend zms --name $name --image $S/builds/$b/$A/zephyr.signed.bin \
       --work $S/work --change "s3 + p256-m + PKA PSA driver + builtin ECP P-256 hook, ECP default ${b##*-}" $files \
       < /dev/null > $S/logs/fabrics-zms-$name.out 2>&1
    echo "rc=$? $(tail -1 $S/logs/fabrics-zms-$name.out | cut -c1-300)"
  done
fi
echo "=== $(date +%H:%M:%S) pka chain2 finished"
