cmake_minimum_required(VERSION 3.20.0)

# The adapter takes the UART through the real coprocessor-manager (on the fake
# UART platform), which needs log-store's header. job-manager and
# coprocessor-updater are registered for the updater ops glue, which is built
# and tested here too. Registered the way the application does it, before
# find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/log-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/coprocessor-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/job-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/coprocessor-updater
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/esp-loader-adapter
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_loader_adapter_test)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fakes)
target_sources(app PRIVATE
    src/main.c
    ../fakes/fake_uart.c
    ../fakes/fake_esp_loader.c
)
