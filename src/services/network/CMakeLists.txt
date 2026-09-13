# The board's network service (network_service.h): network-manager's adapter
# over the W5500 and the ESP-Hosted Wi-Fi driver, its store in settings-registry,
# the five-power-cycle recovery and the worker.
#
# A library of its own rather than part of `app`: `app` is relocated into PSRAM
# (root CMakeLists.txt), and the worker's stack stays in SRAM with the other
# threads that call into drivers.
#
# A plain CMake library linked into `app`, not zephyr_library_named(): in
# application code Zephyr has already read its library list, so a Zephyr
# library declared here is compiled and never linked. Outside --whole-archive
# the linker keeps only objects something references: a file added here that
# holds nothing but SETTING_REGISTRY_DEFINE entries would silently vanish.
add_library(cedar_network_service STATIC
    boot_streak.c
    netcfg_store.c
    net_adapter_map.c
    net_adapter.c
    network_service.c
)
target_link_libraries(cedar_network_service PRIVATE zephyr_interface)
add_dependencies(cedar_network_service zephyr_generated_headers)
target_link_libraries(app PRIVATE cedar_network_service)
