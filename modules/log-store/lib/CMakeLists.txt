# A named library, so the firmware's psram_sections.ld can place its rings
# (.bss of liblog_store.a) in PSRAM.
zephyr_library_named(log_store)

zephyr_library_sources(log_store.c)
