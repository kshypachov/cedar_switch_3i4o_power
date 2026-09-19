# flash-sram-proxy module root.
#   src/ — the cedar,flash-sram-proxy flash driver
# Why and measurements: docs/device-development/reports/littlefs-speed/notes.md
#
# Its own library, not relocated: the proxy thread's stack and the bounce buffer
# must stay in SRAM (the app CMakeLists.txt moves `app` and other libraries to
# PSRAM; src/helpers/psram_sections.ld lists more by archive name).

if(CONFIG_FLASH_SRAM_PROXY)
    zephyr_library_named(flash_sram_proxy)
    zephyr_library_sources(src/flash_sram_proxy.c)
endif()
