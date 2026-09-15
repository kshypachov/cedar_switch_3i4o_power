#!/bin/zsh
# build_one.sh <out-dir> <conf-list ;-separated> [overlay]
set -u
OUT=$1; CONFS=$2; OVL=${3:-}
cd /Volumes/Programming/Zephyr/zephyr_latest
source .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1 ZEPHYR_TOOLCHAIN_VARIANT=zephyr
ARGS=("-Dcedar_switch_3in4out_power_EXTRA_CONF_FILE=$CONFS")
[ -n "$OVL" ] && ARGS+=("-Dcedar_switch_3in4out_power_EXTRA_DTC_OVERLAY_FILE=$OVL")
T0=$(date +%s)
west build -p always -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app --sysbuild -d $OUT cedar_switch_3in4out_power -- "${ARGS[@]}" > $OUT.build.log 2>&1
RC=$?
echo "rc=$RC seconds=$(( $(date +%s) - T0 ))" >> $OUT.build.log
tail -1 $OUT.build.log
