#!/bin/zsh
# 2026-09-14 23:40, owner: open the commissioning window again for Apple Home. Waits for the p256-m
# five-fabric run to finish, then starts an Apple Home session on the firmware as shipped (prod-zms)
# and opens qr.png on the owner's screen.
L=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/apple-home-2.out
echo "=== $(date +%H:%M:%S) apple_after_p256m: waiting for p256m_chain"
while pgrep -f '^/bin/zsh /.*/p256m_chain\.sh' > /dev/null || pgrep -f '[Pp]ython.* /.*/(run_fabrics|crypto_bench_run)\.py' > /dev/null; do sleep 5; done
echo "=== $(date +%H:%M:%S) apple home session (prod-zms)"
/Volumes/Programming/Zephyr/zephyr_latest/.venv/bin/python3 /Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/apple-home/apple_home_session.py /private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning/builds/prod-zms/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin 30 < /dev/null > $L 2>&1 &
for i in $(seq 1 120); do grep -q "QR_READY\|failed\|no QR" $L 2>/dev/null && break; sleep 3; done
P=$(grep -o "QR_READY [^ ]*" $L | cut -d' ' -f2)
[ -n "$P" ] && open "$P"
echo "=== $(date +%H:%M:%S) $(grep -m1 'QR_READY\|failed\|no QR' $L)"
wait
echo "=== $(date +%H:%M:%S) apple home session finished"
