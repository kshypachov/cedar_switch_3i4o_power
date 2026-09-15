# A named library, so the firmware's psram_sections.ld can place its buffers
# (.bss of libsystem_image_store.a: the 16 KiB staging buffer, the read and repair
# buffers and the image check) in PSRAM. Nothing here runs in an ISR; the slot is
# on SPI1 NOR without DMA. The board glue's flash calls copy from these buffers
# into the SPI driver, never into the XIP guard (that writes slot 1 only).
zephyr_library_named(system_image_store)

zephyr_library_sources(
    mcuboot_image.c
    system_image_store.c
)
