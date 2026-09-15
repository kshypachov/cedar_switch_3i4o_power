#!/bin/zsh
# Build one tuning image out of the repository and check the change applied.
#
# usage: build_trial.sh <backend zms|file> <build dir> [trial.conf] [trial.overlay]
#
# The base of each backend is settings-backends/configs: common.conf plus
# zms.conf and settings-on-storage-lfs.overlay for ZMS, file.conf for File.
# A trial adds its own conf/overlay through EXTRA_CONF_FILE /
# EXTRA_DTC_OVERLAY_FILE; the repository is not edited per trial.
#
# Every CONFIG_ line of the trial conf must appear verbatim in the built
# .config (a symbol whose dependencies are not met is dropped silently by
# Kconfig), otherwise the script fails. Overlays are checked by hand against
# zephyr.dts, the property lines are written to <build dir>.check.
set -u
BACKEND=$1; OUT=$2; TCONF=${3:-}; TOVL=${4:-}
HERE=${0:A:h}
CONFIGS=$HERE/../settings-backends/configs
WEST=${HERE:h:h:h:h:h:h}
CONFS="$CONFIGS/common.conf;$CONFIGS/$BACKEND.conf"
OVLS=""
[ "$BACKEND" = zms ] && OVLS="$CONFIGS/settings-on-storage-lfs.overlay"
[ -n "$TCONF" ] && CONFS="$CONFS;$TCONF"
[ -n "$TOVL" ] && OVLS="${OVLS:+$OVLS;}$TOVL"

cd $WEST
source .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1 ZEPHYR_TOOLCHAIN_VARIANT=zephyr
ARGS=("-Dcedar_switch_3in4out_power_EXTRA_CONF_FILE=$CONFS")
[ -n "$OVLS" ] && ARGS+=("-Dcedar_switch_3in4out_power_EXTRA_DTC_OVERLAY_FILE=$OVLS")
T0=$(date +%s)
west build -p always -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app --sysbuild -d $OUT \
	cedar_switch_3in4out_power -- "${ARGS[@]}" > $OUT.build.log 2>&1
RC=$?
echo "rc=$RC seconds=$(( $(date +%s) - T0 ))" | tee -a $OUT.build.log
[ $RC -eq 0 ] || exit $RC

APP=$OUT/cedar_switch_3in4out_power/zephyr
: > $OUT.check
if [ -n "$TCONF" ]; then
	grep -E '^CONFIG_' $TCONF | while read -r line; do
		if [[ $line == *=n ]]; then
			sym=${line%%=*}
			if grep -q "^$sym=" $APP/.config; then echo "NOT APPLIED: $line" | tee -a $OUT.check; RC=3
			else echo "applied: $line ($(grep -E "^# $sym is not set" $APP/.config || echo 'symbol absent'))" >> $OUT.check; fi
		elif ! grep -qxF "$line" $APP/.config; then
			echo "NOT APPLIED: $line (have: $(grep "^${line%%=*}=" $APP/.config))" | tee -a $OUT.check; RC=3
		else
			echo "applied: $line" >> $OUT.check
		fi
	done
fi
if [ -n "$TOVL" ]; then
	echo "--- overlay $TOVL; zephyr.dts has:" >> $OUT.check
	grep -E '^\s*[a-z#,-]+ = ' $TOVL | sed 's/^\s*//; s/;.*//' | while read -r prop; do
		grep -F -- "${prop%% =*} =" $APP/zephyr.dts | sed 's/\/\*.*//' | head -3 >> $OUT.check
	done
fi
grep -q 'NOT APPLIED' $OUT.check && exit 3
exit 0
