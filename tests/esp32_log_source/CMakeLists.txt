cmake_minimum_required(VERSION 3.20.0)

# The assembler alone: bytes in, lines out. It needs log-store's header for the
# level enum only, which the module adds to the include path itself.
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/esp32-log-source
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp32_log_source_test)

target_sources(app PRIVATE src/main.c)

# The ROM stream captured from board B, compiled in: the suite reads it as an
# array, never as a file.
generate_inc_file_for_target(app
    ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/esp32/board-b-rom-link-check.log
    ${ZEPHYR_BINARY_DIR}/include/generated/esp32_rom_fixture.inc
)
