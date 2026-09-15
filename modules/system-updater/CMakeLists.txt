# system-updater module root. Layout mirrors the other in-tree modules:
#   include/system_updater/ — public API (the STM32 install job, its journal,
#                             reconciliation after the swap, self-confirmation)
#   lib/                    — the automaton, in C
# MCUboot (bootutil), the staged image in slot 2, the journal file and the
# reboot are the platform's; the core calls job-manager only, so it builds and
# is tested on native_sim.

zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_SYSTEM_UPDATER lib)
