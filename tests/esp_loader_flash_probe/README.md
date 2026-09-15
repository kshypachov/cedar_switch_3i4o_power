# ESP32-C6 flash probe (P6)

Reads, through the C6's ROM loader, the flash regions the updater's design depends
on: the second-stage bootloader (`0x0`, 4 KiB), the partition table (`0x8000`), the
head of NVS, both otadata copies (`0xd000`, `0xe000`), phy_init and the head of each
app slot (`0x10000`, `0x1d0000`). Prints each region's time, number of `0xFF` bytes,
CRC32 and its non-blank rows, then returns the chip to normal boot.

**Read-only.** The source calls `esp_loader_connect`, `read_mac`,
`flash_detect_size`, `flash_read` and `reset_target` only - no `flash_start`,
`flash_write`, `flash_erase` or `mem_*`. Board B's C6 is kept unflashed until the
web updater writes it (owner's decision), and this probe does not change that.

Build and flash exactly like `tests/esp_loader_integration` (same prj.conf boot
chain and overlay); the result for board B is in `docs/device-development/reports/p6`.
