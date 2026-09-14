# A named library, so the firmware's psram_sections.ld can place its state
# (.bss of libcoprocessor_manager.a) with the other modules' state.
zephyr_library_named(coprocessor_manager)

zephyr_library_sources(coprocessor_manager.c)
