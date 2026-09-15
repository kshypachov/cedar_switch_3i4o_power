#!/bin/zsh
S=${0:A:h}; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning
IFS='|' read -r b n c o <<< "$1"
r=$($T/build_trial.sh $b $S/builds/$n "$c" "$o" 2>&1 | tail -3 | tr '\n' ' ')
echo "$n: $r"
