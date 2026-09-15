#!/usr/bin/env python3
"""Generate the synthetic STM32 (MCUboot) image corpus.

Signed by the workspace's imgtool (bootloader/mcuboot/scripts/imgtool.py) the way
the firmware build signs zephyr.signed.bin - no key, SHA-256 TLV only, header
0x400 - then mutated. Each file is judged by mcubootimage.py while it is written;
a disagreement with the expected outcome fails the run. See README.md.

Run with the workspace's Python (imgtool needs click, cryptography, intelhex,
cbor2):  ../../../../.venv/bin/python gen_stm32_fixtures.py
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import mcubootimage as mi  # noqa: E402

WORKSPACE = HERE.parents[3]
IMGTOOL = Path(os.environ.get("IMGTOOL", WORKSPACE / "bootloader/mcuboot/scripts/imgtool.py"))

SP_OK = 0x200C0000  # the top of the test RAM range: a valid initial SP
RESET_OK = 0x02000405  # Thumb, inside the test execution window


def body(length: int, sp: int = SP_OK, reset: int = RESET_OK) -> bytes:
    filler = bytes((i * 131 + 7) & 0xFF for i in range(length - 8))
    return struct.pack("<II", sp, reset) + filler


def sign(raw: bytes, version: str = "1.2.3+4", header: int = 0x400, extra: list[str] | None = None) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "in.bin"
        dst = Path(tmp) / "out.bin"
        src.write_bytes(raw)
        cmd = [sys.executable, str(IMGTOOL), "sign", "--version", version, "--header-size", hex(header),
               "--pad-header", "--slot-size", hex(0x400000), "--align", "1", *(extra or []), str(src), str(dst)]
        subprocess.run(cmd, check=True, capture_output=True)
        return dst.read_bytes()


def patch(data: bytes, offset: int, value: bytes) -> bytes:
    out = bytearray(data)
    out[offset:offset + len(value)] = value
    return bytes(out)


def tlv_offset(data: bytes, wanted: int) -> int:
    """Offset of the TLV entry of type @wanted in the unprotected area."""
    hdr_size, prot, img = struct.unpack_from("<HHI", data, 8)
    pos = hdr_size + img + prot
    _, tot = struct.unpack_from("<HH", data, pos)
    end = pos + tot
    pos += 4
    while pos < end:
        t, ln = struct.unpack_from("<HH", data, pos)
        if t == wanted:
            return pos
        pos += 4 + ln
    raise ValueError(f"no TLV {wanted:#x}")


def corpus() -> list[tuple[str, bytes, str | None, str | None, str]]:
    valid = sign(body(3000))
    flags = struct.unpack_from("<I", valid, 16)[0]
    return [
        ("valid", valid, None, None, "header 0x400, 3 000-byte body, SHA-256 TLV, version 1.2.3+4"),
        ("valid_large", sign(body(70000), version="2.0.1+77"), None, None,
         "70 000-byte body: several 16 KiB chunks and 4 KiB sectors, version 2.0.1+77"),
        ("valid_prot_tlv", sign(body(3000), extra=["--security-counter", "7"]), None, None,
         "a protected TLV area (security counter) inside the hashed range"),
        ("bad_magic", patch(valid, 0, b"\x3c"), mi.INVALID, "header magic is missing",
         "the first header byte changed"),
        ("header_0x200", sign(body(3000), header=0x200), mi.TARGET, "header size is not the one",
         "signed with a 0x200 header"),
        ("flag_encrypted", patch(valid, 16, struct.pack("<I", flags | 0x4)), mi.INVALID,
         "encrypted, compressed", "IMAGE_F_ENCRYPTED_AES128 set"),
        ("flag_ram_load", patch(valid, 16, struct.pack("<I", flags | 0x20)), mi.INVALID,
         "encrypted, compressed", "IMAGE_F_RAM_LOAD set"),
        ("truncated", valid[:-10], mi.INVALID, "TLV area runs past the end", "the last 10 bytes missing"),
        ("trailing_bytes", valid + b"\xff" * 16, mi.INVALID, "bytes after the image's TLV area",
         "16 erased bytes after the TLV area"),
        ("bad_image_hash", patch(valid, 0x400 + 100, bytes([valid[0x400 + 100] ^ 0xFF])), mi.INVALID,
         "SHA-256 does not match the one in its TLV", "one body byte changed after signing"),
        ("no_sha_tlv", patch(valid, tlv_offset(valid, 0x10), struct.pack("<H", 0x11)), mi.INVALID,
         "no SHA-256 TLV", "the SHA-256 TLV retyped as 0x11"),
        ("bad_prot_tlv", None, mi.INVALID, "protected TLV area is malformed",
         "the protected TLV info magic changed"),
        ("wrong_sp", sign(body(3000, sp=0x10000000)), mi.TARGET, "stack pointer is not in this board's RAM",
         "initial SP 0x10000000"),
        ("wrong_reset", sign(body(3000, reset=0x08000401)), mi.TARGET, "reset vector is not in this board",
         "reset vector 0x08000401"),
        ("reset_even", sign(body(3000, reset=0x02000404)), mi.TARGET, "reset vector is not in this board",
         "reset vector without the Thumb bit"),
    ]


def main() -> int:
    manifest = []
    failures = 0
    prot = sign(body(3000), extra=["--security-counter", "7"])
    hdr_size, prot_size, img = struct.unpack_from("<HHI", prot, 8)
    for name, data, code, fragment, what in corpus():
        if name == "bad_prot_tlv":
            data = patch(prot, hdr_size + img, struct.pack("<H", 0x6909))
        verdict = mi.check(data, mi.Params())
        ok = verdict.code == code and (fragment is None or (verdict.message and fragment in verdict.message))
        if not ok:
            print(f"FAIL {name}: expected {code} '{fragment}', got {verdict.code} '{verdict.message}'")
            failures += 1
        (HERE / f"{name}.bin").write_bytes(data)
        manifest.append({"name": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                         "code": code, "message": fragment, "version": verdict.version,
                         "image_hash": verdict.image_hash.hex() if verdict.image_hash else None,
                         "what": what})
        print(f"{'ok  ' if ok else 'FAIL'} {name:16} {len(data):6} {verdict.code or 'accepted'}")
    (HERE / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
