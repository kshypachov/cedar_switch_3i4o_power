# A named library, so the firmware's psram_sections.ld can place its buffers
# (.bss of libfirmware_store.a: the 16 KiB staging buffer, the read buffer and
# the image check) in PSRAM. Nothing here runs in an ISR or feeds DMA: /lfs is
# on SPI1 NOR without DMA (reports/p6/notes-design-inputs.md).
zephyr_library_named(firmware_store)

zephyr_library_sources(
    fw_md5.c
    fw_image.c
    firmware_store.c
)
