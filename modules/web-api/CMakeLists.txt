# web-api module root:
#   include/web_api/ — request/response values, routing, middleware, JSON
#   lib/             — the core, and the Zephyr HTTP server adapter
# The core has no socket in it and is exercised on native_sim directly; the
# adapter is exercised against Zephyr's real HTTP server over loopback.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_WEB_API lib)
