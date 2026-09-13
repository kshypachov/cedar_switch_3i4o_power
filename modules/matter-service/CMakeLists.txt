# matter-service module root. Layout mirrors the other in-tree modules:
#   include/matter_service/ — public API (snapshot reads, window requests,
#                             adapter reports)
#   lib/                    — the core, in C
# The Matter SDK is not a dependency of this module: the firmware's adapter in
# src/matter reaches it, so the core builds and is tested on native_sim.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_MATTER_SERVICE lib)
