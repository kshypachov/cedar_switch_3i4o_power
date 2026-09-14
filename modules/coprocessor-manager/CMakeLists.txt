# coprocessor-manager module root. Layout mirrors the other in-tree modules:
#   include/coprocessor_manager/ — public API (UART mode, C6 reset, claims)
#   lib/                         — the core, in C
# No driver is a dependency of this module: the board's adapter in
# src/services/coprocessor attaches the UART handlers and drives EN/BOOT, so
# the core builds and is tested on native_sim. The record kinds come from
# log_store.h (types only), so modules/log-store must be registered too.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_COPROCESSOR_MANAGER lib)
