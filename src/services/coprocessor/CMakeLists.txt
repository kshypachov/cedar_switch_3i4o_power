# The board's coprocessor service (coprocessor_service.h): coprocessor-manager's
# platform over USART3, the USB CDC passthrough and EN/BOOT, and the ESP32 log
# source on its worker.
#
# A library of its own rather than part of `app`, for the same reason as the
# network service: `app` is relocated into PSRAM (root CMakeLists.txt), and the
# receive rings the UART and CDC interrupts write, and the worker's stack, stay
# in SRAM. A plain CMake library linked into `app`, not zephyr_library_named():
# in application code Zephyr has already read its library list.
add_library(cedar_coprocessor_service STATIC
    coprocessor_service.c
)
target_link_libraries(cedar_coprocessor_service PRIVATE zephyr_interface)
add_dependencies(cedar_coprocessor_service zephyr_generated_headers)
target_link_libraries(app PRIVATE cedar_coprocessor_service)
