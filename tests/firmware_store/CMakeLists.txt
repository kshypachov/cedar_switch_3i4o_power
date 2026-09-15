cmake_minimum_required(VERSION 3.20.0)

# The module under test is not in the west manifest, so register it the same
# way the application does, before find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/firmware-store
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firmware_store_test)

target_sources(app PRIVATE
    src/main.c
    src/md5.c
    src/image.c
    src/store.c
)

# The synthetic corpus (tests/fixtures/esp32/firmware, gen_firmware_fixtures.py)
# as byte arrays: fw_<name>.inc.
set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/esp32/firmware)
set(GEN_DIR ${ZEPHYR_BINARY_DIR}/include/generated)
foreach(name
    valid valid_16_segments truncated bootloader_wrong_chip app_wrong_chip bare_app other_layout
    bad_table_md5 bad_app_sha junk_in_nvs no_app_desc no_bootloader_desc)
  generate_inc_file_for_target(app ${FIXTURES}/${name}.bin ${GEN_DIR}/fw_${name}.inc)
endforeach()
