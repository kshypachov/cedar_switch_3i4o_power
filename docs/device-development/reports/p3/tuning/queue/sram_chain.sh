#!/bin/zsh
# 2026-09-15, owner: move hot Matter/crypto data and code from PSRAM/XIP into SRAM (standard
# zephyr_code_relocate) and repeat the five-fabric commissioning run on board A.
# Cumulative stages, all built from one tree, same common.conf:
#   sram-c0        control: current tree (upstream W5500 driver), nothing moved
#   sram-s1        + libCHIP.a / libtfpsacrypto.a .bss/.noinit (Matter thread stack, crypto state) in SRAM
#   sram-s2        + hot TF-PSA-Crypto code (bignum, ECP, ECDSA, SHA-256, AES/CCM, ...) as RAM_TEXT
#   sram-s3        + libc malloc arena (Matter, mbedTLS) in SRAM (ARENA_SIZE=-1: the rest of SRAM)
#   sram-s3-p256m  s3 + p256-m (+PSA_WANT_ALG_JPAKE)
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning
T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
A=cedar_switch_3in4out_power/zephyr
typeset -A change
change[sram-c0]="control for the SRAM move: current tree (upstream W5500), prod ZMS 4 sectors, polled SPI"
change[sram-s1]="+ libCHIP/libtfpsacrypto .bss/.noinit in SRAM (Matter thread stack, crypto state)"
change[sram-s2]="s1 + hot TF-PSA-Crypto code in SRAM (zephyr_code_relocate RAM_TEXT)"
change[sram-s3]="s2 + libc malloc arena in SRAM (rest of SRAM)"
change[sram-s3-p256m]="s3 + p256-m (+PSA_WANT_ALG_JPAKE)"
for b in ${=@}; do
  img=$S/builds/$b/$A/zephyr.signed.bin
  [ -f $img ] || { echo "=== $b: no image"; continue; }
  files=()
  [[ $b == *p256m ]] && files=(--files $S/trials/crypto/p256m.conf)
  echo "=== $(date +%H:%M:%S) fabrics zms-$b"
  $PY $T/fabrics/run_fabrics.py --backend zms --name $b --image $img --work $S/work \
     --change "${change[$b]}" $files < /dev/null > $S/logs/fabrics-zms-$b.out 2>&1
  echo "rc=$? $(tail -1 $S/logs/fabrics-zms-$b.out | cut -c1-300)"
done
echo "=== $(date +%H:%M:%S) sram chain finished"
