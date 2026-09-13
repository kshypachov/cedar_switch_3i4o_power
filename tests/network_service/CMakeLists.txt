cmake_minimum_required(VERSION 3.20.0)

# The parts of the board's network service that need no network stack: the
# store's slots over a key-value interface, the boot count of the physical
# recovery, and the adapter's translations. The adapter itself is hardware-tier.
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/api-validation
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/job-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/device-config-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/network-manager
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(network_service_test)

set(SERVICE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/services/network)
target_include_directories(app PRIVATE ${SERVICE_DIR})
target_sources(app PRIVATE
    src/main.c
    ${SERVICE_DIR}/boot_streak.c
    ${SERVICE_DIR}/netcfg_store.c
    ${SERVICE_DIR}/net_adapter_map.c
)
