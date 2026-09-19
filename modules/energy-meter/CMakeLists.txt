# energy-meter module root.
#   include/energy_meter/ — public API
#   src/eeprom_flash.c    — the cedar,eeprom-flash flash driver (FRAM for ZMS)
#   src/hlw8032.c         — HLW8032 frame check and parse, no Zephyr dependency
#   src/energy_meter.c    — UART, values, the energy counter in ZMS on the FRAM
# Plan and results: docs/device-development/reports/energy
#
# Its own library, not relocated as a whole: the work queue's stack and the
# UART DMA buffers are .noinit and stay in SRAM. Its .bss (the 16 KiB
# struct zms_fs) goes to PSRAM through src/helpers/psram_sections.ld.

zephyr_include_directories(include)

if(CONFIG_EEPROM_FLASH OR CONFIG_ENERGY_METER)
    zephyr_library_named(energy_meter)
    zephyr_library_sources_ifdef(CONFIG_EEPROM_FLASH src/eeprom_flash.c)
    zephyr_library_sources_ifdef(CONFIG_ENERGY_METER src/energy_meter.c src/hlw8032.c)
endif()
