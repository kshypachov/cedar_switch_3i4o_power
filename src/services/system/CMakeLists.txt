# The board's system service (system_service.h): the MCUboot slots as the
# application sees them, the reset cause, the independent watchdog and the
# `sysupd` shell commands.
#
# A library of its own rather than part of `app`, as the other services: `app` is
# relocated into PSRAM (root CMakeLists.txt), while the watchdog thread's stack
# and the copy buffer must stay in SRAM. The XIP flash driver's guarded write
# bounces a source buffer only when it is outside internal SRAM and runs with
# OCTOSPI1 out of memory-mapped mode; keeping everything it touches in SRAM
# keeps PSRAM out of that window altogether.
add_library(cedar_system_service STATIC
    system_service.c
)
target_link_libraries(cedar_system_service PRIVATE zephyr_interface)
# bootutil's public headers (swap state of a slot); zephyr's own dfu/boot links the same.
target_link_libraries(cedar_system_service PRIVATE MCUBOOT_BOOTUTIL)
add_dependencies(cedar_system_service zephyr_generated_headers)
target_link_libraries(app PRIVATE cedar_system_service)
