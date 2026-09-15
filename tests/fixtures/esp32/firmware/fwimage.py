# SPDX-License-Identifier: Apache-2.0
"""ESP32-C6 full-flash images: build synthetic ones, merge real ones, check either.

The formats are ESP-IDF 5.5.5's (bootloader_support/include/esp_app_format.h,
esp_app_format/include/esp_app_desc.h, esp_bootloader_format/include/
esp_bootloader_desc.h, bootloader_support/include/esp_flash_partitions.h). The
check mirrors modules/firmware-store/lib/fw_image.c rule for rule, so the corpus
the device's sim tests use is also judged by an independent implementation, and a
real merged file can be judged on the host before it goes near a board.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass

TABLE_OFFSET = 0x8000
TABLE_MAX_LEN = 0xC00
DATA_OFFSET = 0x9000
APP_OFFSET = 0x10000
MAX_BYTES = 0x1D0000
CHIP_ESP32C6 = 13

APP_DESC_MAGIC = 0xABCD5432
BOOT_DESC_MAGIC = 80

# partitions_eh_cp_ota_4m.csv of the CP project, as the device's allowlist.
LAYOUT = [
    ("nvs", 0x01, 0x02, 0x9000, 0x4000, 0),
    ("otadata", 0x01, 0x00, 0xD000, 0x2000, 0),
    ("phy_init", 0x01, 0x01, 0xF000, 0x1000, 0),
    ("ota_0", 0x00, 0x10, 0x10000, 0x1C0000, 0),
    ("ota_1", 0x00, 0x11, 0x1D0000, 0x1C0000, 0),
]
LAYOUT_ID = "cedar-c6-ota-4m-2x1792k"
HOST_PROTOCOL = "esp-hosted-mcu-3"


# -- building ---------------------------------------------------------------------


def esp_image(segments: list[tuple[int, bytes]], chip: int = CHIP_ESP32C6,
              hash_appended: int = 1) -> bytes:
    """An ESP image: header, segments, padding with the XOR checksum, SHA-256."""
    header = struct.pack(
        "<BBBBI", 0xE9, len(segments), 2, 0x20, 0x40800000
    ) + struct.pack("<B3sHBHH4sB", 0xEE, b"\0\0\0", chip, 0, 0, 99, b"\0" * 4, hash_appended)
    assert len(header) == 24
    body = bytearray(header)
    checksum = 0xEF
    for load, data in segments:
        body += struct.pack("<II", load, len(data)) + data
        for b in data:
            checksum ^= b
    pad = 15 - (len(body) % 16)
    body += b"\0" * pad + bytes([checksum])
    if hash_appended:
        body += hashlib.sha256(body).digest()
    return bytes(body)


def bootloader_desc(idf: str = "v5.5.5-synthetic") -> bytes:
    desc = struct.pack("<B2sBI32s24s16s", BOOT_DESC_MAGIC, b"\0\0", 0, 1,
                       idf.encode(), b"Sep 14 2026 10:00:00", b"\0" * 16)
    assert len(desc) == 80
    return desc


def app_desc(version: str = "1.2.3-synthetic", project: str = "eh_cp_c6_cedar",
             idf: str = "v5.5.5-synthetic") -> bytes:
    desc = struct.pack("<II8s32s32s16s16s32s32sHHB3s72s", APP_DESC_MAGIC, 0, b"\0" * 8,
                       version.encode(), project.encode(), b"10:00:00", b"Sep 14 2026",
                       idf.encode(), hashlib.sha256(b"elf").digest(), 0, 0xFFFF, 16,
                       b"\0" * 3, b"\0" * 72)
    assert len(desc) == 256
    return desc


def filler(n: int, seed: int) -> bytes:
    return bytes((seed * 31 + i * 7) & 0xFF for i in range(n))


def bootloader(chip: int = CHIP_ESP32C6, desc: bytes | None = None) -> bytes:
    first = (bootloader_desc() if desc is None else desc) + filler(100, 1)
    return esp_image([(0x40875730, first), (0x4086B910, filler(77, 2))], chip=chip)


def application(chip: int = CHIP_ESP32C6, desc: bytes | None = None) -> bytes:
    first = (app_desc() if desc is None else desc) + filler(211, 3)
    return esp_image([(0x420F0020, first), (0x40800000, filler(150, 4)),
                      (0x42000020, filler(333, 5))], chip=chip)


def partition_table(layout=LAYOUT, md5: bool = True, corrupt_md5: bool = False) -> bytes:
    table = bytearray()
    for label, ptype, subtype, offset, size, flags in layout:
        table += struct.pack("<HBBII16sI", 0x50AA, ptype, subtype, offset, size,
                             label.encode(), flags)
    if md5:
        digest = hashlib.md5(table).digest()
        if corrupt_md5:
            digest = bytes([digest[0] ^ 1]) + digest[1:]
        table += b"\xEB\xEB" + b"\xFF" * 14 + digest
    table += b"\xFF" * (TABLE_MAX_LEN - len(table))
    return bytes(table)


def merge(parts: list[tuple[int, bytes]], size: int | None = None) -> bytes:
    """idf.py merge-bin -f raw: each part at its offset, 0xFF between."""
    end = max(offset + len(data) for offset, data in parts)
    out = bytearray(b"\xFF" * (size if size is not None else end))
    for offset, data in parts:
        out[offset:offset + len(data)] = data
    return bytes(out)


def merged(boot: bytes | None = None, table: bytes | None = None,
           app: bytes | None = None) -> bytes:
    return merge([(0, bootloader() if boot is None else boot),
                  (TABLE_OFFSET, partition_table() if table is None else table),
                  (APP_OFFSET, application() if app is None else app)])


# -- checking -----------------------------------------------------------------------


class Reject(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


@dataclass
class Info:
    version: str
    project_name: str
    idf_version: str
    layout_id: str | None
    bootloader_bytes: int
    app_bytes: int
    sha256: str


def _text(field: bytes, allow_empty: bool) -> str:
    end = field.find(b"\0")
    if end < 0:
        raise Reject("invalid_image", "The application description does not hold text")
    value = field[:end]
    if (not value and not allow_empty) or any(b < 0x20 or b > 0x7E for b in value):
        raise Reject("invalid_image", "The application description does not hold text")
    return value.decode()


def _walk(data: bytes, base: int, region_end: int, is_boot: bool) -> int:
    """Check the ESP image at @base; return its end."""
    if len(data) < base + 24 or data[base] != 0xE9:
        raise Reject("invalid_image", "No ESP image header at 0x0" if is_boot
                     else "No application image header at 0x10000")
    count = data[base + 1]
    chip = struct.unpack_from("<H", data, base + 12)[0]
    if chip != CHIP_ESP32C6:
        raise Reject("unsupported_target", "The bootloader is built for another chip"
                     if is_boot else "The application is built for another chip")
    if count == 0 or count > 16:
        raise Reject("invalid_image", "An image has an impossible segment count")
    if data[base + 23] != 1:
        raise Reject("invalid_image", "An image carries no appended SHA-256")
    pos = base + 24
    checksum = 0xEF
    for index in range(count):
        if pos + 8 > region_end:
            raise Reject("invalid_image", "The bootloader runs past 0x8000" if is_boot
                         else "The application is cut short")
        length = struct.unpack_from("<I", data, pos + 4)[0]
        desc_len = 80 if is_boot else 176
        if length > MAX_BYTES:
            raise Reject("invalid_image", "A bootloader segment runs past 0x8000" if is_boot
                         else "An application segment runs past the end of the file")
        if index == 0:
            # Only the bootloader's first segment may overrun: the description
            # decides what the file is before the region's end does.
            if not is_boot and pos + 8 + length > region_end:
                raise Reject("invalid_image",
                             "An application segment runs past the end of the file")
            if length < desc_len:
                raise Reject("invalid_image",
                             "The first segment is too short to hold the image description")
            desc = data[pos + 8:pos + 8 + desc_len]
            if len(desc) == desc_len:
                if is_boot and desc[0] != BOOT_DESC_MAGIC:
                    if struct.unpack_from("<I", desc)[0] == APP_DESC_MAGIC:
                        raise Reject("invalid_image", "This is an application image, "
                                     "not the full-flash file (idf.py merge-bin)")
                    raise Reject("invalid_image",
                                 "The bootloader at 0x0 has no bootloader description")
                if not is_boot and struct.unpack_from("<I", desc)[0] != APP_DESC_MAGIC:
                    raise Reject("invalid_image",
                                 "The application at 0x10000 has no application description")
        elif pos + 8 + length > region_end:
            raise Reject("invalid_image", "A bootloader segment runs past 0x8000" if is_boot
                         else "An application segment runs past the end of the file")
        pos += 8
        if pos + length > region_end:
            raise Reject("invalid_image", "The bootloader runs past 0x8000" if is_boot
                         else "The application is cut short")
        for b in data[pos:pos + length]:
            checksum ^= b
        pos += length
    checksum_at = base + ((pos - base) // 16 + 1) * 16 - 1
    if checksum_at + 33 > region_end:
        raise Reject("invalid_image", "The bootloader runs past 0x8000" if is_boot
                     else "The application is cut short")
    if data[checksum_at] != checksum:
        raise Reject("invalid_image", "The bootloader's checksum does not match" if is_boot
                     else "The application's checksum does not match")
    if hashlib.sha256(data[base:checksum_at + 1]).digest() != data[checksum_at + 1:checksum_at + 33]:
        raise Reject("invalid_image",
                     "The bootloader's SHA-256 does not match its contents" if is_boot
                     else "The application's SHA-256 does not match its contents")
    return checksum_at + 33


def check(data: bytes) -> Info:
    """Raise Reject(code, message) or return what the file says about itself."""
    if len(data) > MAX_BYTES:
        raise Reject("invalid_image", "The file reaches ota_1 at 0x1d0000")
    boot_end = _walk(data, 0, min(len(data), TABLE_OFFSET), True)
    if any(b != 0xFF for b in data[boot_end:TABLE_OFFSET]):
        raise Reject("invalid_image",
                     "Bytes between the bootloader and the partition table are not erased")
    table = data[TABLE_OFFSET:TABLE_OFFSET + TABLE_MAX_LEN]
    if len(table) < TABLE_MAX_LEN:
        raise Reject("invalid_image", "The file ends before the application at 0x10000")
    entries = []
    md5_seen = False
    layout_id = None
    for i in range(0, TABLE_MAX_LEN, 32):
        e = table[i:i + 32]
        magic = struct.unpack_from("<H", e)[0]
        if magic == 0x50AA and not md5_seen:
            entries.append(e)
            continue
        if magic == 0xEBEB and not md5_seen and entries:
            if hashlib.md5(table[:i]).digest() != e[16:32]:
                raise Reject("invalid_image", "The partition table's MD5 record does not match")
            md5_seen = True
            continue
        if e == b"\xFF" * 32 and entries:
            if not md5_seen:
                raise Reject("invalid_image", "The partition table has no MD5 record")
            if any(b != 0xFF for b in table[i:]):
                raise Reject("invalid_image",
                             "Bytes after the partition table's end are not erased")
            parsed = [(x[12:28].rstrip(b"\0").decode(errors="replace"), x[2], x[3],
                       *struct.unpack_from("<II", x, 4), struct.unpack_from("<I", x, 28)[0])
                      for x in entries]
            layout_id = LAYOUT_ID if parsed == LAYOUT and all(
                x[12:28] == label.encode().ljust(16, b"\0")
                for x, (label, *_rest) in zip(entries, LAYOUT)) else None
            break
        raise Reject("invalid_image", "No partition table at 0x8000" if i == 0
                     else "The partition table has a corrupt entry")
    else:
        raise Reject("invalid_image", "The partition table has no end")
    if any(b != 0xFF for b in data[TABLE_OFFSET + TABLE_MAX_LEN:DATA_OFFSET]):
        raise Reject("invalid_image", "Bytes after the partition table are not erased")
    if any(b != 0xFF for b in data[DATA_OFFSET:APP_OFFSET]):
        raise Reject("invalid_image",
                     "NVS, otadata and phy_init (0x9000-0x10000) are not erased")
    if len(data) <= APP_OFFSET:
        raise Reject("invalid_image", "The file ends before the application at 0x10000")
    app_end = _walk(data, APP_OFFSET, len(data), False)
    if app_end != len(data):
        raise Reject("invalid_image", "Bytes follow the end of the application")
    desc = data[APP_OFFSET + 32:APP_OFFSET + 32 + 176]
    info = Info(_text(desc[16:48], False), _text(desc[48:80], False), _text(desc[112:144], True),
                layout_id, boot_end, app_end - APP_OFFSET, hashlib.sha256(data).hexdigest())
    if layout_id is None:
        raise Reject("incompatible_firmware",
                     "The partition table's layout is not one this device supports")
    return info
