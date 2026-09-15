# Storage report

Measures how much room `/lfs` really has, which decides whether the staged
ESP32-C6 image needs a bigger partition. Opens all four relays first, then
idles, so it is safe to leave running on a bench board.

It mounts the same LittleFS the application does - the fstab node is copied
from the application's board overlay, not invented here - so the numbers
reflect the real filesystem.

## Result on a freshly flashed board, 2026-09-12

```
storage_nvs    off=0x00a00000 size=5120 KiB   <- /lfs lives here
storage_lfs    off=0x00000000 size=6144 KiB   <- unused, free reserve
image-1        off=0x00600000 size=4096 KiB
/lfs: total=5120 KiB  free=4992 KiB  used=128 KiB  (64 KiB blocks, 80 total)
```

Since 2026-09-14 the 6 MiB partition at offset 0 is renamed `storage_zms` and holds
the settings store (ZMS, `zephyr,settings-partition`); it is no longer a free
reserve, and the test now prints it under the new name.

A 1.75 MiB C6 image fits with room to spare, so **the partition does not need
to grow** for the first version.

Two caveats before treating this as final: the filesystem was empty apart from
LittleFS metadata, so a board in service will show less, and the allocation
unit is a 64 KiB erase block, so a staged image costs about 28 blocks rather
than its exact byte size. The server must still enforce its own quota instead
of trusting free space at upload time.

## Build and run

```sh
west build -p always \
  -b cedar_switch_3in4out_power_rev3/stm32u585xx/ext_flash_app \
  cedar_switch_3in4out_power/tests/storage_report
```
