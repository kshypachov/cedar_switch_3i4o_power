#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Turn the frontend's dist directory into the table web-assets serves.

Everything about a file that does not depend on the request is decided here,
once, at build time, and written two ways: as C for the firmware, and as a JSON
manifest for the mock server, which serves the same bytes with the same headers
while the frontend's end-to-end tests run. The policy is documented in
modules/web-assets/include/web_assets/web_assets.h; this is its build half.

Decisions made here:

- **MIME types come from a closed list.** A file with an extension not on it
  fails the build instead of reaching a browser as application/octet-stream,
  which for a script means a page that does not run and says nothing useful.
- **One stored representation per file.** Text that gzip shrinks by at least a
  tenth, and that is at least 1 KiB to begin with, is stored gzipped only;
  everything else identity only. Below 1 KiB the saving is a few hundred bytes
  and the file becomes unreadable to a tool that does not decode it, and
  index.html - the file a tool is likely to fetch - is usually that small.
- **Reproducible output.** Files are walked in sorted order, gzip gets a zero
  mtime and no file name, and nothing else varies, so the same dist produces the
  same C source byte for byte. A firmware image built twice from one commit is
  the same image.
- **Strong ETags over the stored bytes**, sixteen hex digits of SHA-256. Stored
  bytes rather than the original, because the ETag names a representation, and
  a gzipped file and its identity form are different representations.
- **Immutable caching only for hashed names.** Vite puts a content hash in the
  name of what it emits under assets/; only such a name can be cached for a
  year, because only such a name changes when the content does.
- **Dotfiles are not served.** The build stamp and Vite's own manifest live in
  dist/ and are for tooling, not browsers.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

#: Extension -> (Content-Type, compressible).
MIME_TYPES: dict[str, tuple[str, bool]] = {
    ".html": ("text/html; charset=utf-8", True),
    ".js": ("text/javascript; charset=utf-8", True),
    ".mjs": ("text/javascript; charset=utf-8", True),
    ".css": ("text/css; charset=utf-8", True),
    ".json": ("application/json", True),
    ".map": ("application/json", True),
    ".webmanifest": ("application/manifest+json", True),
    ".svg": ("image/svg+xml", True),
    ".txt": ("text/plain; charset=utf-8", True),
    ".png": ("image/png", False),
    ".ico": ("image/x-icon", False),
    ".woff2": ("font/woff2", False),
}

GZIP_MIN_BYTES = 1024
GZIP_MAX_RATIO = 0.9

#: Vite's default output name: name-<hash>.ext, the hash at least 8 characters.
HASHED_NAME = re.compile(r"^/assets/.+-[A-Za-z0-9_-]{8,}\.[A-Za-z0-9]+$")

#: What a URL path of ours may contain; anything else is refused at build time
#: rather than escaped, because the C table and the URL must agree exactly.
SAFE_PATH = re.compile(r"^/[A-Za-z0-9._/-]+$")

CACHE_REVALIDATE = "revalidate"
CACHE_IMMUTABLE = "immutable"


class AssetError(Exception):
    """A dist directory the firmware must not embed."""


@dataclass(frozen=True)
class Asset:
    path: str
    source: str
    content_type: str
    data: bytes
    gzip: bool
    etag: str
    cache: str
    is_page: bool
    original_size: int


def _gzip(data: bytes) -> bytes:
    return gzip.compress(data, compresslevel=9, mtime=0)


def build_asset(path: str, source: str, data: bytes) -> Asset:
    if not SAFE_PATH.match(path) or "//" in path or "/../" in path or path.endswith("/.."):
        raise AssetError(f"{path!r}: not a path the device can serve")
    ext = Path(path).suffix.lower()
    if ext not in MIME_TYPES:
        raise AssetError(f"{path!r}: extension {ext or '(none)'} is not on the MIME list")
    content_type, compressible = MIME_TYPES[ext]

    stored = data
    use_gzip = False
    if compressible and len(data) >= GZIP_MIN_BYTES:
        packed = _gzip(data)
        if len(packed) <= len(data) * GZIP_MAX_RATIO:
            stored = packed
            use_gzip = True

    etag = '"' + hashlib.sha256(stored).hexdigest()[:16] + '"'
    cache = CACHE_IMMUTABLE if HASHED_NAME.match(path) else CACHE_REVALIDATE
    return Asset(
        path=path,
        source=source,
        content_type=content_type,
        data=stored,
        gzip=use_gzip,
        etag=etag,
        cache=cache,
        is_page=path == "/index.html",
        original_size=len(data),
    )


def collect(dist: Path) -> list[Asset]:
    if not (dist / "index.html").is_file():
        raise AssetError(f"{dist}: no index.html - is this a built frontend?")
    assets = []
    for file in sorted(p for p in dist.rglob("*") if p.is_file()):
        rel = file.relative_to(dist)
        if any(part.startswith(".") for part in rel.parts):
            continue
        path = "/" + rel.as_posix()
        assets.append(build_asset(path, rel.as_posix(), file.read_bytes()))
    # Sorted the way strcmp() sorts, which is what the C lookup bisects by.
    assets.sort(key=lambda a: a.path.encode())
    return assets


def read_version(dist: Path, override: str | None) -> str:
    if override:
        return override
    stamp = dist / ".cedar-build.json"
    if stamp.is_file():
        version = json.loads(stamp.read_text()).get("version")
        if isinstance(version, str) and version:
            return version
    return "unknown"


def _c_string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_c(assets: list[Asset], version: str) -> str:
    lines = [
        "/* Generated by tools/web-assets/gen_web_assets.py. Do not edit. */",
        "",
        "#include <web_assets/web_assets.h>",
        "",
    ]
    for i, asset in enumerate(assets):
        lines.append(f"/* {asset.path} ({asset.original_size} bytes"
                     f"{', gzip ' + str(len(asset.data)) if asset.gzip else ''}) */")
        lines.append(f"static const uint8_t asset_{i}[] = {{")
        for off in range(0, len(asset.data), 16):
            chunk = asset.data[off:off + 16]
            lines.append("\t" + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
        if not asset.data:
            lines.append("\t0x00, /* empty file; len is 0 */")
        lines.append("};")
        lines.append("")
    lines.append("static const struct web_asset assets[] = {")
    index = None
    for i, asset in enumerate(assets):
        if asset.is_page:
            index = i
        lines.append("\t{")
        lines.append(f"\t\t.path = {_c_string(asset.path)},")
        lines.append(f"\t\t.content_type = {_c_string(asset.content_type)},")
        lines.append(f"\t\t.etag = {_c_string(asset.etag)},")
        lines.append(f"\t\t.data = asset_{i},")
        lines.append(f"\t\t.len = {len(asset.data)},")
        lines.append(f"\t\t.gzip = {'true' if asset.gzip else 'false'},")
        cache = "WEB_ASSET_CACHE_IMMUTABLE" if asset.cache == CACHE_IMMUTABLE else "WEB_ASSET_CACHE_REVALIDATE"
        lines.append(f"\t\t.cache = {cache},")
        lines.append(f"\t\t.is_page = {'true' if asset.is_page else 'false'},")
        lines.append("\t},")
    lines.append("};")
    lines.append("")
    lines.append("const struct web_assets_table web_assets = {")
    lines.append("\t.assets = assets,")
    lines.append(f"\t.count = {len(assets)},")
    lines.append(f"\t.index = &assets[{index}],")
    lines.append(f"\t.version = {_c_string(version)},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def render_manifest(assets: list[Asset], version: str) -> str:
    return json.dumps(
        {
            "version": version,
            "assets": [
                {
                    "path": a.path,
                    "source": a.source,
                    "content_type": a.content_type,
                    "etag": a.etag,
                    "gzip": a.gzip,
                    "cache": a.cache,
                    "is_page": a.is_page,
                    "stored_bytes": len(a.data),
                    "original_bytes": a.original_size,
                }
                for a in assets
            ],
        },
        indent=2,
        sort_keys=True,
    ) + "\n"


def write_if_changed(path: Path, text: str) -> None:
    """Leave an identical file untouched, so the build does not recompile it."""
    if path.exists() and path.read_text() == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def page_csp_from_c(source: str) -> str:
    """The Content-Security-Policy literal from web_assets.c.

    The mock serves the page with the policy the device sends. Reading it from
    the C source, rather than keeping a second copy here, means the two cannot
    disagree without a test failing.
    """
    match = re.search(r"web_assets_page_csp\s*=\s*((?:\s*\"[^\"]*\")+)\s*;", source)
    if not match:
        raise AssetError("web_assets.c: web_assets_page_csp literal not found")
    return "".join(re.findall(r'"([^"]*)"', match.group(1)))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--out-c", type=Path)
    parser.add_argument("--out-manifest", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--budget-kib", type=int, default=0,
                        help="fail if the stored total exceeds this many KiB (0: no limit)")
    args = parser.parse_args(argv)

    try:
        assets = collect(args.dist)
    except AssetError as exc:
        print(f"gen_web_assets: {exc}", file=sys.stderr)
        return 1
    version = read_version(args.dist, args.version)
    total = sum(len(a.data) for a in assets)
    if args.budget_kib and total > args.budget_kib * 1024:
        print(f"gen_web_assets: {total} bytes stored exceeds the budget of "
              f"{args.budget_kib} KiB", file=sys.stderr)
        return 1
    if args.out_c:
        write_if_changed(args.out_c, render_c(assets, version))
    if args.out_manifest:
        write_if_changed(args.out_manifest, render_manifest(assets, version))
    print(f"gen_web_assets: {len(assets)} files, {total} bytes stored, version {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
