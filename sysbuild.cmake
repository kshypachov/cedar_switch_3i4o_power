# MCUboot (sysbuild image "mcuboot") получает boot_go_hook платы отдельным
# Zephyr-модулем: ICACHE remap 0x02000000 -> 0x90000000 (приложение слинковано
# на алиас внешней QSPI) и подтверждение образа по паттерну в BBRAM.
set(mcuboot_EXTRA_ZEPHYR_MODULES
    "${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_hooks"
    CACHE INTERNAL "MCUboot board boot hooks"
)
