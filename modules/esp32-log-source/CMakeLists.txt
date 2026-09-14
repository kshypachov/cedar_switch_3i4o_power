# esp32-log-source module root. Layout mirrors the other in-tree modules:
#   include/esp32_log_source/ — public API (the line assembler)
#   lib/                      — the assembler, in C
# The UART is not a dependency of this module: the board's coprocessor service
# owns the interrupt and feeds bytes in, so the assembler builds and is tested on
# native_sim.
#
# The header uses log-store's level enum only, so log-store's include directory is
# added here too: a suite of the assembler alone does not have to build log-store.

zephyr_include_directories(include ${CMAKE_CURRENT_LIST_DIR}/../log-store/include)

add_subdirectory_ifdef(CONFIG_ESP32_LOG_SOURCE lib)
