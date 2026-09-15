cmake_minimum_required(VERSION 3.20.0)

# The module under test is not in the west manifest, so register it the same
# way the application does, before find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/system-image-store
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(system_image_store_test)

target_sources(app PRIVATE
    src/main.c
    src/image.c
    src/store.c
)

# The synthetic corpus (tests/fixtures/stm32, gen_stm32_fixtures.py) as byte
# arrays: img_<name>.inc; and one ESP32-C6 merged file as a foreign format.
set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/stm32)
set(GEN_DIR ${ZEPHYR_BINARY_DIR}/include/generated)
foreach(name
    valid valid_large valid_prot_tlv bad_magic header_0x200 flag_encrypted flag_ram_load
    truncated trailing_bytes bad_image_hash no_sha_tlv bad_prot_tlv wrong_sp wrong_reset reset_even)
  generate_inc_file_for_target(app ${FIXTURES}/${name}.bin ${GEN_DIR}/img_${name}.inc)
endforeach()
generate_inc_file_for_target(app
    ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/esp32/firmware/valid.bin ${GEN_DIR}/img_esp32_merged.inc)
