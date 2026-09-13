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

# Both fakes are shared: the web API suite drives the network bindings over the
# same interface fake.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fakes)
target_sources(app PRIVATE
    src/main.c
    ../fakes/fake_iface.c
    ../fakes/fake_storage.c
)
