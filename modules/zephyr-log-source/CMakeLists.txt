# zephyr-log-source module root. Layout mirrors the other in-tree modules:
#   include/zephyr_log_source/ — statistics and the text sanitiser (for tests)
#   lib/                       — the log backend, in C
# The backend is Zephyr's own interface, so the module builds and is tested on
# native_sim with deferred logging and the real log-store.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_ZEPHYR_LOG_SOURCE lib)
