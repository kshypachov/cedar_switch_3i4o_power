# STM32 (MCUboot) image corpus

Synthetic `zephyr.signed.bin`-style images for the device's STM32 image check
(`modules/system-image-store`, `mcuboot_image.h`) and for API and bench runs that
need a file the device must refuse. Signed by the workspace's imgtool
(`bootloader/mcuboot/scripts/imgtool.py sign --header-size 0x400 --pad-header
--slot-size 0x400000 --align 1`, no key: SHA-256 TLV only, as the firmware build
signs), then mutated. Real application images are not kept here (1.4 MiB).

The body is a vector table (initial SP `0x200C0000`, reset `0x02000405`) and
filler. The "board" they are judged against is the test's parameters: header
`0x400`, RAM `(0x20000000, 0x200C0000]`, execution window
`[0x02000000, 0x02400000)` - the same as `tests/system_image_store/src/common.h`.

| File | Expected | What it exercises |
|---|---|---|
| `valid.bin` | accepted | 3 000-byte body, version `1.2.3+4` |
| `valid_large.bin` | accepted | 70 000-byte body (several 16 KiB chunks, 4 KiB sectors), version `2.0.1+77` |
| `valid_prot_tlv.bin` | accepted | a protected TLV area (security counter) inside the hashed range |
| `bad_magic.bin` | `invalid_image` | the first header byte changed |
| `header_0x200.bin` | `unsupported_target` | signed with a 0x200 header |
| `flag_encrypted.bin` | `invalid_image` | `IMAGE_F_ENCRYPTED_AES128` set |
| `flag_ram_load.bin` | `invalid_image` | `IMAGE_F_RAM_LOAD` set |
| `truncated.bin` | `invalid_image` | the last 10 bytes missing |
| `trailing_bytes.bin` | `invalid_image` | 16 erased bytes after the TLV area |
| `bad_image_hash.bin` | `invalid_image` | one body byte changed after signing |
| `no_sha_tlv.bin` | `invalid_image` | the SHA-256 TLV retyped as 0x11 |
| `bad_prot_tlv.bin` | `invalid_image` | the protected TLV info magic changed |
| `wrong_sp.bin` | `unsupported_target` | initial SP `0x10000000` |
| `wrong_reset.bin` | `unsupported_target` | reset vector `0x08000401` |
| `reset_even.bin` | `unsupported_target` | reset vector without the Thumb bit |

Sizes, SHA-256, image hashes and message fragments are in `manifest.json`.

## Regenerate

```sh
cd tests/fixtures/stm32
../../../../.venv/bin/python gen_stm32_fixtures.py
```

imgtool needs `click`, `cryptography`, `intelhex` and `cbor2`, which the
workspace venv has. Each file is judged while it is written by
`mcubootimage.py`, an independent Python reading of the rules (from
`bootutil/image.h` and `bootutil_img_validate()`); a disagreement with the
expected outcome fails the run. imgtool without a key is deterministic, so a
regeneration reproduces the files byte for byte. `tests/system_image_store`
embeds them and keeps the same expectations in `src/main.c` - change both
together.

Judge a real build on the host (with the board's parameters edited into
`Params` when they differ):

```sh
python3 -c "import mcubootimage as m; print(m.check(open('zephyr.signed.bin','rb').read(), m.Params()))"
```
