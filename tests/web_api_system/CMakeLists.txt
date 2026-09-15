cmake_minimum_required(VERSION 3.20.0)

# The STM32 update branches of the v1 bindings (firmware.c by target and id,
# system_update.c) through the real route table, over the real firmware-store,
# system-image-store and system-updater. tests/web_api builds the same bindings
# without the STM32 modules, so those branches compile out there.
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/api-validation
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/job-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/web-auth
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/web-api
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/matter-service
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/device-config-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/network-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/log-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/coprocessor-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/firmware-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/coprocessor-updater
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/system-image-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/system-updater
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(web_api_system_test)

# The real v1 bindings and the web-api suites' harness (request values in,
# response read back, fake web-auth platform).
set(V1_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/web/api/v1)
target_include_directories(app PRIVATE
    ${V1_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../web_api/src
)
target_sources(app PRIVATE
    src/system.c
    ../web_api/src/main.c
    ../fakes/fake_iface.c
    ../fakes/fake_storage.c
    ../fakes/fake_system_platform.c
    ${V1_DIR}/router.c
    ${V1_DIR}/auth.c
    ${V1_DIR}/system.c
    ${V1_DIR}/matter.c
    ${V1_DIR}/network.c
    ${V1_DIR}/logs.c
    ${V1_DIR}/coprocessor.c
    ${V1_DIR}/firmware.c
    ${V1_DIR}/system_update.c
)

# Images of the synthetic STM32 corpus (tests/fixtures/stm32) as byte arrays.
set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/stm32)
set(GEN_DIR ${ZEPHYR_BINARY_DIR}/include/generated)
foreach(name valid valid_large bad_magic wrong_sp)
  generate_inc_file_for_target(app ${FIXTURES}/${name}.bin ${GEN_DIR}/img_${name}.inc)
endforeach()
