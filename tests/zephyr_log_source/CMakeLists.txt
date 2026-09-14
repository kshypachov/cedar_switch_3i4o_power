cmake_minimum_required(VERSION 3.20.0)

# The backend with the real log-store and the real deferred log core.
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/log-store
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/zephyr-log-source
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_log_source_test)

target_sources(app PRIVATE src/main.c src/first.c)
