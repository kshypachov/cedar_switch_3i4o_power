# web-auth module root. Layout mirrors the other in-tree modules:
#   include/web_auth/ — public API (setup, login, sessions, CSRF, password change)
#   lib/              — the core, plus the PSA and settings-registry adapters
# The core has no hardware, crypto library or storage dependency of its own;
# those arrive through struct web_auth_platform, so the whole state machine is
# exercised on native_sim with fakes.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_WEB_AUTH lib)
