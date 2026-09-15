# SPDX-License-Identifier: Apache-2.0
"""The STM32 image check `verifyUpload` runs on a `stm32u585` upload.

The file is `zephyr.signed.bin`: an MCUboot header, the application, and the TLV
area imgtool appends (api-contract.md, "Обновление STM32"). The mock parses the
bytes it received rather than consulting a scenario knob, because the frontend's
job here is to send the file intact and let the device judge it; a mock that
accepted anything would hide a client that corrupts the upload.

What is checked, in this order, and what each failure is called:

1. magic `0x96f3b83d` — `invalid_image` (not an MCUboot image at all);
2. `ih_hdr_size` is this board's header size (`CONFIG_ROM_START_OFFSET`, 0x400) —
   `unsupported_target`: an image built for another board or layout;
3. no flag the device cannot boot: PIC, encryption, RAM load, non-bootable,
   compression — `invalid_image`;
4. the protected TLV area (if any) and the TLV area are where the header says,
   and the TLV area ends exactly at the end of the file — `invalid_image`;
5. the SHA-256 TLV is present and equals the digest of header, body and
   protected TLV area, which is the check MCUboot itself makes — `invalid_image`;
6. the vector table: the initial stack pointer lies in SRAM or PSRAM and the
   reset vector is a Thumb address inside the application window —
   `unsupported_target`.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass

IMAGE_MAGIC = 0x96F3B83D
TLV_INFO_MAGIC = 0x6907
TLV_PROT_INFO_MAGIC = 0x6908
TLV_SHA256 = 0x10
#: CONFIG_ROM_START_OFFSET of the board's application build.
HEADER_SIZE = 0x400
#: IMAGE_F_PIC, ENCRYPTED_AES128/256, NON_BOOTABLE, RAM_LOAD, COMPRESSED_*.
FORBIDDEN_FLAGS = 0x001 | 0x004 | 0x008 | 0x010 | 0x020 | 0x200 | 0x400 | 0x800
#: Where an initial stack pointer may point: internal SRAM, or the PSRAM window
#: MCUboot leaves mapped. A stack pointer is the address past the stack, so the
#: upper bound is included and the lower one is not.
STACK_RANGES = ((0x2000_0000, 0x200C_0000), (0x7000_0000, 0x7080_0000))
#: The application window: the 4 MiB alias of slot 1, past the header.
CODE_START = 0x0200_0000 + HEADER_SIZE
CODE_END = 0x0240_0000


@dataclass(frozen=True)
class McubootImage:
    version: str
    #: The TLV SHA-256, hex: what the running image is identified by afterwards.
    image_hash: str


class ImageRejected(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def version_key(version: str) -> tuple[int, int, int, int]:
    """`major.minor.revision+build` as a tuple that orders like MCUboot's
    comparison with the build number included."""
    core, _, build = version.partition("+")
    parts = [int(p) for p in core.split(".")]
    while len(parts) < 3:
        parts.append(0)
    return (parts[0], parts[1], parts[2], int(build or 0))


def _invalid(message: str) -> ImageRejected:
    return ImageRejected("invalid_image", message)


def check(data: bytes) -> McubootImage:
    """The image, or `ImageRejected` with the contract's code."""
    if len(data) < 32 or struct.unpack_from("<I", data, 0)[0] != IMAGE_MAGIC:
        raise _invalid("The file is not an MCUboot image (zephyr.signed.bin): no image header")
    (_load, hdr_size, prot_size, img_size, flags, major, minor, revision, build) = struct.unpack_from(
        "<IHHIIBBHI", data, 4
    )
    if hdr_size != HEADER_SIZE:
        raise ImageRejected(
            "unsupported_target",
            f"The image header is {hdr_size} bytes; this board's firmware has {HEADER_SIZE}",
        )
    if flags & FORBIDDEN_FLAGS:
        raise _invalid(f"The image has flags this device cannot boot (0x{flags:08x})")

    body_end = hdr_size + img_size
    if prot_size:
        if body_end + 4 > len(data):
            raise _invalid("The protected TLV area lies past the end of the file")
        magic, total = struct.unpack_from("<HH", data, body_end)
        if magic != TLV_PROT_INFO_MAGIC or total != prot_size:
            raise _invalid("The protected TLV area does not match the header")
    tlv_off = body_end + prot_size
    if tlv_off + 4 > len(data):
        raise _invalid("The TLV area lies past the end of the file")
    magic, tlv_total = struct.unpack_from("<HH", data, tlv_off)
    if magic != TLV_INFO_MAGIC:
        raise _invalid("The TLV area is missing where the header says it starts")
    if tlv_off + tlv_total != len(data):
        raise _invalid("The file does not end where the image's TLV area ends")

    digest = None
    off = tlv_off + 4
    while off < len(data):
        if off + 4 > len(data):
            raise _invalid("A TLV entry is cut off")
        kind, length = struct.unpack_from("<HH", data, off)
        if off + 4 + length > len(data):
            raise _invalid("A TLV entry runs past the TLV area")
        if kind == TLV_SHA256:
            # MCUboot refuses a digest entry of the wrong size rather than
            # looking for another one.
            if length != 32:
                raise _invalid("The image's SHA-256 entry has the wrong length")
            digest = data[off + 4 : off + 36]
        off += 4 + length
    if digest is None:
        raise _invalid("The image carries no SHA-256")
    if hashlib.sha256(data[:tlv_off]).digest() != digest:
        raise _invalid("The image's SHA-256 does not match its contents")

    if img_size < 8:
        raise _invalid("The image is too short to hold a vector table")
    stack, reset = struct.unpack_from("<II", data, hdr_size)
    stack_ok = any(low < stack <= high for low, high in STACK_RANGES)
    reset_ok = bool(reset & 1) and CODE_START <= (reset & ~1) < CODE_END
    if not (stack_ok and reset_ok):
        raise ImageRejected(
            "unsupported_target",
            "The image's vector table does not point into this board's memory",
        )
    return McubootImage(
        version=f"{major}.{minor}.{revision}+{build}",
        image_hash=digest.hex(),
    )


def build_image(
    body: bytes = b"",
    *,
    version: tuple[int, int, int, int] = (1, 0, 1, 0),
    header_size: int = HEADER_SIZE,
    flags: int = 0,
    stack: int = 0x200C_0000,
    reset: int = CODE_START + 0x101,
    protected: bytes = b"",
    extra_tlvs: bytes = b"",
    corrupt_hash: bool = False,
    vectors: bool = True,
) -> bytes:
    """A small image the way imgtool lays one out, for tests and fixtures.

    `body` follows the two vector table words (none with `vectors=False`);
    `protected` is the payload of a protected TLV area (entries included), empty
    for none; `extra_tlvs` are raw bytes appended after the SHA-256 entry.
    """
    image_body = (struct.pack("<II", stack, reset) if vectors else b"") + body
    prot = b""
    if protected:
        prot = struct.pack("<HH", TLV_PROT_INFO_MAGIC, 4 + len(protected)) + protected
    header = struct.pack(
        "<IIHHIIBBHII",
        IMAGE_MAGIC,
        0,
        header_size,
        len(prot),
        len(image_body),
        flags,
        version[0],
        version[1],
        version[2],
        version[3],
        0,
    )
    header += b"\xff" * (header_size - len(header))
    digest = bytearray(hashlib.sha256(header + image_body + prot).digest())
    if corrupt_hash:
        digest[0] ^= 0xFF
    entries = struct.pack("<HH", TLV_SHA256, 32) + bytes(digest) + extra_tlvs
    tlvs = struct.pack("<HH", TLV_INFO_MAGIC, 4 + len(entries)) + entries
    return header + image_body + prot + tlvs
