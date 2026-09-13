cmake_minimum_required(VERSION 3.20.0)

# The modules under test are not in the west manifest, so register them the
# same way the application does, before find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/api-validation
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/job-manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/web-auth
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/web-api
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/matter-service
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(web_api_test)

# The real v1 bindings, not copies: the middleware is tested through the route
# table the device serves.
set(V1_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/web/api/v1)
target_include_directories(app PRIVATE ${V1_DIR})
target_sources(app PRIVATE
    src/main.c
    src/middleware.c
    src/v1.c
    src/json_reader.c
    src/json_writer.c
    ${V1_DIR}/router.c
    ${V1_DIR}/auth.c
    ${V1_DIR}/system.c
    ${V1_DIR}/matter.c
)
