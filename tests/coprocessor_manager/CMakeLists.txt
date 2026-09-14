cmake_minimum_required(VERSION 3.20.0)

# coprocessor-manager takes its record kinds from log_store.h, so log-store is
# registered for its header; CONFIG_LOG_STORE stays off and its library is not
# built. Both are registered the way the application does it, before
# find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/log-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/coprocessor-manager
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(coprocessor_manager_test)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fakes)
target_sources(app PRIVATE
    src/main.c
    ../fakes/fake_uart.c
)
