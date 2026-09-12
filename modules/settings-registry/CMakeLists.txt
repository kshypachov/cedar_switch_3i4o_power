# Settings registry module root. Layout mirrors the other in-tree modules:
#   include/settings_registry/ — public API (per-key get/set/subscribe/enumerate)
#   lib/                       — registry implementation
# Plain C throughout; transport-agnostic — web/shell/Matter consumers sit on top.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_SETTINGS_REGISTRY lib)
