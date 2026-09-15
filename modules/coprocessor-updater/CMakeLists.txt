# coprocessor-updater module root. Layout mirrors the other in-tree modules:
#   include/coprocessor_updater/ — public API (one install job, its journal)
#   lib/                         — the phase automaton, in C
# The flasher, the staged image, the journal file and ESP-Hosted are the
# platform's; the core calls job-manager and reads coprocessor-manager's
# status, so it builds and is tested on native_sim.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_COPROCESSOR_UPDATER lib)
