# Device configuration store module root. Layout mirrors the other in-tree
# modules:
#   include/device_config_store/ — public API (generations, secrets, recovery)
#   lib/                         — implementation
# Plain C with an injected storage backend, so the whole module — power loss
# and failing writes included — is exercised on native_sim.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_DEVICE_CONFIG_STORE lib)
