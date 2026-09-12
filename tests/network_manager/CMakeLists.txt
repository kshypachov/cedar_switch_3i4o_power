cmake_minimum_required(VERSION 3.20.0)

# network-manager sits on top of the other three, so all four are registered
# the same way the application does it, before find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/api-validation
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/job-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/device-config-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/network-manager
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(network_manager_test)

# The flash fake is shared; the interface fake is specific to this suite.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fakes)
target_sources(app PRIVATE
    src/main.c
    src/fake_iface.c
    ../fakes/fake_storage.c
)
