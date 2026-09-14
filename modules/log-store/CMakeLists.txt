# log-store module root. Layout mirrors the other in-tree modules:
#   include/log_store/ — public API (append, readers, cursors, counters)
#   lib/               — the implementation
# No dependency on the logging subsystem: the Zephyr log backend and the ESP32
# UART source feed this store, so it must never log itself.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_LOG_STORE lib)
