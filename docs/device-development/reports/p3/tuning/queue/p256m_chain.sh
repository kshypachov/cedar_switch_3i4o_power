#!/bin/zsh
# 2026-09-14 23:30, owner's request: pairing speed with p256-m. Same firmware as zms-prod-sectors4
# (ZMS 4 sectors, polled SPI, settings shell) + CONFIG_MBEDTLS_PSA_P256M_DRIVER_ENABLED + PSA_WANT_ALG_JPAKE
# (keeps builtin ECP for Matter's SPAKE2+). 1) crypto_bench baseline, 2) crypto_bench p256-m, 3) five fabrics.
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning; R=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports; PY=/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3
A=cedar_switch_3in4out_power/zephyr
echo "=== $(date +%H:%M:%S) p256m_chain: crypto_bench baseline (prod-zms-shell)"
$PY $R/crypto/bench/crypto_bench_run.py prod-zms-shell $S/builds/prod-zms-shell/$A/zephyr.signed.bin 10 < /dev/null > $S/logs/cbench-base.out 2>&1
echo "rc=$? $(tail -1 $S/logs/cbench-base.out | cut -c1-400)"
echo "=== $(date +%H:%M:%S) crypto_bench p256-m (prod-zms-shell-p256m)"
$PY $R/crypto/bench/crypto_bench_run.py prod-zms-shell-p256m $S/builds/prod-zms-shell-p256m/$A/zephyr.signed.bin 10 < /dev/null > $S/logs/cbench-p256m.out 2>&1
echo "rc=$? $(tail -1 $S/logs/cbench-p256m.out | cut -c1-400)"
echo "=== $(date +%H:%M:%S) fabrics zms-prod-sectors4-p256m"
$PY $R/p3/tuning/fabrics/run_fabrics.py --backend zms --name prod-sectors4-p256m    --image $S/builds/prod-zms-shell-p256m/$A/zephyr.signed.bin --work $S/work    --change "prod ZMS 4 sectors + p256-m (+PSA_WANT_ALG_JPAKE)" --files $S/trials/crypto/p256m.conf    < /dev/null > $S/logs/fabrics-zms-prod-sectors4-p256m.out 2>&1
echo "rc=$? $(tail -1 $S/logs/fabrics-zms-prod-sectors4-p256m.out | cut -c1-300)"
echo "=== $(date +%H:%M:%S) p256m check finished"
