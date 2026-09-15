# ESP32-C6 full-flash image corpus

Synthetic `raw_full_flash` files (the format of `idf.py merge-bin`) for the
device's image check (`modules/firmware-store`, `fw_image.h`) and for the API and
bench runs that need a file the device must refuse. Real CP images are not kept
here: they are 1.4 MiB each and come from the neighbouring CP project.

| File | Expected | What it exercises |
|---|---|---|
| `valid.bin` | accepted | bootloader (336 B, `bootloader_desc`), partition table (layout `cedar-c6-ota-4m-2x1792k`, MD5 record), blank NVS/otadata/phy_init, application (1 040 B, `app_desc` version `1.2.3-synthetic`) |
| `valid_16_segments.bin` | accepted | an application with 16 segments, the most an ESP image allows |
| `truncated.bin` | `invalid_image` | the last 100 bytes missing |
| `bootloader_wrong_chip.bin` | `unsupported_target` | chip id 5 in the bootloader header |
| `app_wrong_chip.bin` | `unsupported_target` | chip id 5 in the application header |
| `bare_app.bin` | `invalid_image` | an application `.bin` instead of the merged file |
| `other_layout.bin` | `incompatible_firmware` | a well-formed table with a 1 MiB ota_0 |
| `bad_table_md5.bin` | `invalid_image` | the table's MD5 record off by one bit |
| `bad_app_sha.bin` | `invalid_image` | the application's appended SHA-256 altered |
| `junk_in_nvs.bin` | `invalid_image` | one byte of NVS not erased |
| `no_app_desc.bin` | `invalid_image` | no `esp_app_desc_t` |
| `no_bootloader_desc.bin` | `invalid_image` | no `esp_bootloader_desc_t` |

Sizes, SHA-256 and the exact messages are in `manifest.json`. Formats: ESP-IDF
5.5.5, `bootloader_support/include/esp_app_format.h`,
`esp_app_format/include/esp_app_desc.h`,
`esp_bootloader_format/include/esp_bootloader_desc.h`,
`bootloader_support/include/esp_flash_partitions.h`.

## Regenerate

```sh
cd tests/fixtures/esp32/firmware
python3 gen_firmware_fixtures.py
```

Each file is judged while it is written by `fwimage.py`, an independent Python
implementation of the device's rules; a disagreement with the expected outcome
fails the run. `tests/firmware_store` embeds the files and keeps the same
expectations in `src/main.c` - change both together.

The one case too large for the repository, a file reaching ota_1 at `0x1d0000`:

```sh
python3 gen_firmware_fixtures.py --past-ota1 /tmp/past-ota1.bin
```

## Real CP images

Merge a CP build the way `idf.py merge-bin -f raw` does (parts from
`flasher_args.json`, 0xFF between them) and judge the result on the host:

```sh
python3 gen_firmware_fixtures.py --merge <esp32c6-hosted-cp>/build -o merged-wifi.bin
python3 gen_firmware_fixtures.py --check merged-wifi.bin
```

Measured 2026-09-14 (`docs/device-development/reports/p6/notes-firmware-store.md`):
the Wi-Fi CP build merges to 1 468 176 bytes, sha256 `750cb58e…cb77`; an OT CP
application with the same bootloader and table, 869 056 bytes, `5bbe0d27…7a16`.
Both are accepted - the check cannot tell the two CP variants apart (owner's
decision №4 deferred).
