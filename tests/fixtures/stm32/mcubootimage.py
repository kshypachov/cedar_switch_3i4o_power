"""An independent Python reading of the device's MCUboot image rules.

The rules are modules/system-image-store (mcuboot_image.h): the generator judges
every fixture with this module while writing it, and a disagreement with the
expected outcome fails the run. Written from bootutil/image.h and
bootutil_img_validate(), not from the C file.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field

IMAGE_MAGIC = 0x96F3B83D
TLV_INFO_MAGIC = 0x6907
TLV_PROT_INFO_MAGIC = 0x6908
TLV_SHA256 = 0x10
HEADER_SIZE = 32
REFUSED_FLAGS = 0x4 | 0x8 | 0x10 | 0x20 | 0x200 | 0x400 | 0x800
TLV_MAX = 2048

INVALID = "invalid_image"
TARGET = "unsupported_target"


@dataclass
class Params:
    header_size: int = 0x400
    ram: list[tuple[int, int]] = field(default_factory=lambda: [(0x20000000, 0x200C0000)])
    exec_start: int = 0x02000000
    exec_end: int = 0x02400000


@dataclass
class Verdict:
    code: str | None
    message: str | None
    version: str | None = None
    image_hash: bytes | None = None


def check(data: bytes, p: Params) -> Verdict:
    size = len(data)
    if size < HEADER_SIZE + 8 + 4:
        return Verdict(INVALID, "The file is too small to be an MCUboot image")
    magic, _load, hdr_size, prot_size, img_size, flags = struct.unpack_from("<IIHHII", data, 0)
    major, minor, revision, build = struct.unpack_from("<BBHI", data, 20)
    if magic != IMAGE_MAGIC:
        return Verdict(INVALID, "This is not an MCUboot image: the header magic is missing")
    if hdr_size != p.header_size:
        return Verdict(TARGET, "The image's header size is not the one this board's firmware is built with")
    if flags & REFUSED_FLAGS:
        return Verdict(INVALID, "The image is encrypted, compressed, loaded into RAM or not bootable")
    if img_size < 8:
        return Verdict(INVALID, "The image has no vector table")
    tlv_start = hdr_size + img_size
    hash_end = tlv_start + prot_size
    if hash_end + 4 > size:
        return Verdict(INVALID, "The image's header declares more bytes than the file has")
    if size - tlv_start > TLV_MAX:
        return Verdict(INVALID, "The file continues too far past the image")

    area = data[tlv_start:]
    off = 0
    if prot_size:
        pm, ptot = struct.unpack_from("<HH", area, 0) if prot_size >= 4 else (0, 0)
        if prot_size < 4 or pm != TLV_PROT_INFO_MAGIC or ptot != prot_size:
            return Verdict(INVALID, "The image's protected TLV area is malformed")
        off = prot_size
    m, tot = struct.unpack_from("<HH", area, off)
    if m != TLV_INFO_MAGIC:
        return Verdict(INVALID, "The image has no TLV area after its body")
    end = off + tot
    if end > len(area):
        return Verdict(INVALID, "The image's TLV area runs past the end of the file")
    if end < len(area):
        return Verdict(INVALID, "The file has bytes after the image's TLV area")
    tlv_hash = None
    pos = off + 4
    while pos < end:
        if pos + 4 > end:
            return Verdict(INVALID, "The image's TLV area is malformed")
        t, ln = struct.unpack_from("<HH", area, pos)
        if pos + 4 + ln > end:
            return Verdict(INVALID, "The image's TLV area is malformed")
        if t == TLV_SHA256:
            if ln != 32:
                return Verdict(INVALID, "The image's SHA-256 TLV has the wrong length")
            tlv_hash = area[pos + 4:pos + 4 + ln]
        pos += 4 + ln
    if tlv_hash is None:
        return Verdict(INVALID, "The image has no SHA-256 TLV")
    if hashlib.sha256(data[:hash_end]).digest() != tlv_hash:
        return Verdict(INVALID, "The image's SHA-256 does not match the one in its TLV area")

    sp, reset = struct.unpack_from("<II", data, hdr_size)
    if not any(start < sp <= stop for start, stop in p.ram):
        return Verdict(TARGET, "The image's initial stack pointer is not in this board's RAM")
    handler = reset & ~1
    if not (reset & 1) or not (p.exec_start <= handler < p.exec_end):
        return Verdict(TARGET, "The image's reset vector is not in this board's application flash")
    return Verdict(None, None, f"{major}.{minor}.{revision}+{build}", tlv_hash)
