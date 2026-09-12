cmake_minimum_required(VERSION 3.20.0)

# The module under test is not in the west manifest, so register it the same
# way the application does, before find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/job-manager
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(job_manager_test)

target_sources(app PRIVATE src/main.c)
