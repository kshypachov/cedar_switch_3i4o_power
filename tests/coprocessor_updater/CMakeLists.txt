cmake_minimum_required(VERSION 3.20.0)

# The updater over a fake platform (loader, image, journal, console evidence,
# ESP-Hosted) with the real job-manager and the real coprocessor-manager on
# the fake UART, whose mode it reads. log-store for the manager's header.
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/log-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/coprocessor-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/job-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/coprocessor-updater
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(coprocessor_updater_test)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fakes)
target_sources(app PRIVATE
    src/main.c
    ../fakes/fake_uart.c
    ../fakes/fake_update_platform.c
)
