cmake_minimum_required(VERSION 3.20.0)

# The modules under test are not in the west manifest, so register them the
# same way the application does, before find_package(Zephyr).
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
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(web_api_test)

# The real v1 bindings, not copies: the middleware is tested through the route
# table the device serves. The network bindings run over network-manager with
# the shared interface and storage fakes.
set(V1_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/web/api/v1)
target_include_directories(app PRIVATE ${V1_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../fakes)
target_sources(app PRIVATE
    src/main.c
    src/middleware.c
    src/v1.c
    src/v1_logs.c
    src/v1_coredump.c
    src/v1_firmware.c
    src/json_reader.c
    src/json_writer.c
    ../fakes/fake_iface.c
    ../fakes/fake_storage.c
    ../fakes/fake_update_platform.c
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

# Two files of the synthetic corpus (tests/fixtures/esp32/firmware) as byte arrays,
# for the upload bindings driven end to end (src/v1_firmware.c).
set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/esp32/firmware)
set(GEN_DIR ${ZEPHYR_BINARY_DIR}/include/generated)
foreach(name valid bare_app)
  generate_inc_file_for_target(app ${FIXTURES}/${name}.bin ${GEN_DIR}/fw_${name}.inc)
endforeach()
