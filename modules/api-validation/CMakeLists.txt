# API validation module root. Layout mirrors the other in-tree modules:
#   include/api_validation/ — public API (error codes, rejections, parsers)
#   lib/                    — implementation
# Plain C with no I/O and no networking dependency, so the whole module is
# exercised on native_sim.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_API_VALIDATION lib)
