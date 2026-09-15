#!/bin/zsh
# 2026-09-14 16:40, owner's request: ZMS 4 sectors (production prj.conf) + SPI interrupt on five fabrics.
# Waits for the polled production run, builds while the board is idle, checks the .config differs from
# prod-zms-shell by exactly CONFIG_SPI_STM32_INTERRUPT=y, logs the driver signature, runs run_fabrics.
set -u
S=/private/tmp/claude-501/-Volumes-Programming-Zephyr-zephyr-latest-cedar-switch-3in4out-power/cb63d62f-514a-4642-ade5-91d00007b397/scratchpad/tuning; T=/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning; W=/Volumes/Programming/Zephyr/zephyr_latest
PY=$W/.venv/bin/python3; NM=$HOME/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm
C=$W/cedar_switch_3in4out_power/docs/device-development/reports/p3/settings-backends/configs
echo "=== $(date +%H:%M:%S) prod_interrupt_chain: waiting for the polled production run"
sleep 5
while pgrep -f '^/bin/zsh /.*/prod_verify_chain\.sh' > /dev/null || pgrep -f '[Pp]ython.* /.*/(run_trial|run_fabrics|prod_boot_check)\.py' > /dev/null; do sleep 10; done
echo "=== $(date +%H:%M:%S) building prod-zms-shell-int"
cd $W && source .venv/bin/activate && export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1 ZEPHYR_TOOLCHAIN_VARIANT=zephyr
west build -p always -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app --sysbuild -d $S/builds/prod-zms-shell-int cedar_switch_3in4out_power -- "-Dcedar_switch_3in4out_power_EXTRA_CONF_FILE=$C/common.conf;$S/trials/fabrics2/spi_interrupt.conf" > $S/builds/prod-zms-shell-int.build.log 2>&1 || { echo "=== build FAILED"; exit 1; }
A=cedar_switch_3in4out_power/zephyr
d=$(diff <(grep -v '^#\|^$' $S/builds/prod-zms-shell/$A/.config | sort) <(grep -v '^#\|^$' $S/builds/prod-zms-shell-int/$A/.config | sort) | grep '^[<>]')
echo "=== .config diff vs prod-zms-shell: $d"
[ "$d" = "> CONFIG_SPI_STM32_INTERRUPT=y" ] || { echo "=== unexpected .config difference, stopping"; exit 1; }
sig() { $NM -S $S/builds/$1/$A/zephyr.elf | grep -E ' (w5500_|esp_hosted_mcu_)' | awk '{print $4, $2}' | sort | shasum | cut -c1-12; }
echo "=== driver signature: polled $(sig prod-zms-shell), interrupt $(sig prod-zms-shell-int)"
echo "=== $(date +%H:%M:%S) fabrics zms-prod-sectors4-spi_interrupt"
$PY $T/fabrics/run_fabrics.py --backend zms --name prod-sectors4-spi_interrupt    --image $S/builds/prod-zms-shell-int/$A/zephyr.signed.bin --work $S/work    --change "prj.conf ZMS 4 sectors, cache 2048 + CONFIG_SPI_STM32_INTERRUPT=y" --files $S/trials/fabrics2/spi_interrupt.conf    < /dev/null > $S/logs/fabrics-zms-prod-sectors4-spi_interrupt.out 2>&1
echo "rc=$? $(tail -1 $S/logs/fabrics-zms-prod-sectors4-spi_interrupt.out | cut -c1-300)"
echo "=== $(date +%H:%M:%S) prod interrupt check finished"
