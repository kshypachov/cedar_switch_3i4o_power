# Network manager module root. Layout mirrors the other in-tree modules:
#   include/network_manager/ — public API (status, transaction, scan)
#   lib/                     — implementation
# Interfaces are reached through an injected ops table and time through an
# injected clock, so the whole transaction runs on native_sim.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_NETWORK_MANAGER lib)
