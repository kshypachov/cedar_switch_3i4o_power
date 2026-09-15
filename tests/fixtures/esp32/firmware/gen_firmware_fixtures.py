#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the synthetic full-flash corpus, or merge and check a real CP build.

    gen_firmware_fixtures.py                 regenerate the corpus next to this file
    gen_firmware_fixtures.py --check FILE    judge FILE as the device would
    gen_firmware_fixtures.py --merge BUILD -o OUT
                                             merge an ESP-IDF build directory
                                             (bootloader 0x0, table 0x8000,
                                             ota_data_initial 0xd000, app 0x10000)
                                             like `idf.py merge-bin -f raw`, then check it
    gen_firmware_fixtures.py --past-ota1 OUT write the one corpus case too large to
                                             keep in the repository (1.9 MiB)

Every corpus file is checked by fwimage.check() while it is written, and the
expected outcome lands in manifest.json, which tests/firmware_store also reads
(as a C table it keeps in step: see tests/firmware_store/src/corpus.h).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import fwimage as fw

HERE = Path(__file__).resolve().parent


def with_byte(data: bytes, offset: int, value: int) -> bytes:
    out = bytearray(data)
    out[offset] = value
    return bytes(out)


def corpus() -> dict[str, tuple[bytes, str | None, str]]:
    """name -> (bytes, expected code or None, what it exercises)."""
    valid = fw.merged()
    app = fw.application()
    boot = fw.bootloader()
    other_layout = [list(e) for e in fw.LAYOUT]
    other_layout[3][4] = 0x100000  # ota_0 of 1 MiB
    other_layout[4][3] = 0x110000
    no_app_desc = fw.application(desc=b"\0" * 256)
    no_boot_desc = fw.bootloader(desc=b"\0" * 80)

    # The app's digest is its last 32 bytes: flip the very last one.
    bad_sha = with_byte(valid, len(valid) - 1, valid[-1] ^ 0x01)

    sixteen = fw.esp_image([(0x420F0020, fw.app_desc() + fw.filler(20, 9))] +
                           [(0x40800000 + i, fw.filler(17 + i, 10 + i)) for i in range(15)])

    return {
        "valid.bin": (valid, None,
                      "bootloader, table, blank otadata and app at their offsets"),
        "valid_16_segments.bin": (fw.merged(app=sixteen), None,
                                  "an application with the most segments an ESP image allows"),
        "truncated.bin": (valid[:-100], "invalid_image",
                          "the last 100 bytes of the application missing"),
        "bootloader_wrong_chip.bin": (fw.merged(boot=fw.bootloader(chip=5)),
                                      "unsupported_target",
                                      "bootloader header names chip 5 (ESP32-C3)"),
        "app_wrong_chip.bin": (fw.merged(app=fw.application(chip=5)), "unsupported_target",
                               "application header names chip 5"),
        "bare_app.bin": (app, "invalid_image", "an application .bin instead of the merged file"),
        "other_layout.bin": (fw.merged(table=fw.partition_table(
            layout=[tuple(e) for e in other_layout])), "incompatible_firmware",
                             "a well-formed table with a 1 MiB ota_0"),
        "bad_table_md5.bin": (fw.merged(table=fw.partition_table(corrupt_md5=True)),
                              "invalid_image", "the table's MD5 record off by one bit"),
        "bad_app_sha.bin": (bad_sha, "invalid_image", "the application's appended SHA-256 altered"),
        "junk_in_nvs.bin": (with_byte(valid, 0x9000, 0x00), "invalid_image",
                            "one byte of NVS not erased"),
        "no_app_desc.bin": (fw.merged(app=no_app_desc), "invalid_image",
                            "application without esp_app_desc_t"),
        "no_bootloader_desc.bin": (fw.merged(boot=no_boot_desc), "invalid_image",
                                   "bootloader without esp_bootloader_desc_t"),
    }


def outcome(data: bytes) -> tuple[str | None, str, fw.Info | None]:
    try:
        info = fw.check(data)
        return None, "", info
    except fw.Reject as r:
        return r.code, str(r), None


def regenerate() -> int:
    manifest = {}
    failures = 0
    for name, (data, expected, why) in corpus().items():
        code, message, info = outcome(data)
        if code != expected:
            print(f"{name}: expected {expected}, the host check says {code}: {message}")
            failures += 1
        (HERE / name).write_bytes(data)
        manifest[name] = {
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "expected": expected,
            "message": message or None,
            "exercises": why,
        }
        if info is not None:
            manifest[name]["image"] = {
                "version": info.version, "project_name": info.project_name,
                "idf_version": info.idf_version, "layout_id": info.layout_id,
                "bootloader_bytes": info.bootloader_bytes, "app_bytes": info.app_bytes,
            }
    (HERE / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"{len(manifest)} files, {failures} disagreements")
    return 1 if failures else 0


def merge_build(build: Path, out: Path) -> int:
    flasher = json.loads((build / "flasher_args.json").read_text())
    parts = [(int(offset, 16), (build / name).read_bytes())
             for offset, name in flasher["flash_files"].items()]
    data = fw.merge(parts)
    out.write_bytes(data)
    print(f"{out}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}")
    for offset, part in sorted(parts):
        print(f"  0x{offset:06x} {len(part):8d} bytes sha256 {hashlib.sha256(part).hexdigest()}")
    return check_file(out)


def check_file(path: Path) -> int:
    code, message, info = outcome(path.read_bytes())
    if code is None:
        print(f"{path}: accepted - {info}")
        return 0
    print(f"{path}: {code}: {message}")
    return 2


def past_ota1(out: Path) -> int:
    base = fw.merged()
    data = base + b"\xFF" * (fw.MAX_BYTES + 1 - len(base))
    out.write_bytes(data)
    return check_file(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", type=Path)
    parser.add_argument("--merge", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--past-ota1", type=Path)
    args = parser.parse_args()
    if args.check:
        return check_file(args.check)
    if args.merge:
        if not args.output:
            parser.error("--merge needs -o")
        return merge_build(args.merge, args.output)
    if args.past_ota1:
        return past_ota1(args.past_ota1)
    return regenerate()


if __name__ == "__main__":
    sys.exit(main())
