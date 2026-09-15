# A named library, so the firmware's psram_sections.ld can place its state.
zephyr_library_named(coprocessor_updater)

zephyr_library_sources(coprocessor_updater.c)
