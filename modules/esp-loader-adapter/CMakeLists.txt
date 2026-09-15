# esp-loader-adapter module root. Layout mirrors the other in-tree modules:
#   include/esp_loader_adapter/ — public API (a flashing session with cleanup)
#   lib/                        — the session core; the table over esp-serial-flasher
#                                 (CONFIG_ESP_LOADER_ADAPTER_ZEPHYR) and the ops
#                                 coprocessor-updater takes (CONFIG_ESP_LOADER_ADAPTER_UPDATER_OPS)
# The core calls coprocessor-manager for the UART and nothing else outside
# this module, so it builds and is tested on native_sim with a fake library.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_ESP_LOADER_ADAPTER lib)
