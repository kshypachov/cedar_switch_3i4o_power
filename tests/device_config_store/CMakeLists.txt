cmake_minimum_required(VERSION 3.20.0)

# The module under test is not in the west manifest, so register it the same
# way the application does, before find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/device-config-store
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(device_config_store_test)

# The flash fake is shared with any other suite that needs durable storage;
# section 12 of the plan puts the common fakes under tests/.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fakes)
target_sources(app PRIVATE
    src/main.c
    ../fakes/fake_storage.c
)
