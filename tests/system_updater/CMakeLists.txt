cmake_minimum_required(VERSION 3.20.0)

# The updater over a fake platform (running and staged image, MCUboot's
# request and confirmation, the journal, the restart) with the real job-manager.
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/job-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/system-updater
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(system_updater_test)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fakes)
target_sources(app PRIVATE
    src/main.c
    ../fakes/fake_system_platform.c
)
