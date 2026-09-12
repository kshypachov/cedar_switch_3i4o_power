# Job manager module root. Layout mirrors the other in-tree modules:
#   include/job_manager/ — public API (create, state, progress, idempotency)
#   lib/                 — implementation
# Plain C, no hardware dependencies and no I/O, so the whole module is
# exercised on native_sim.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_JOB_MANAGER lib)
