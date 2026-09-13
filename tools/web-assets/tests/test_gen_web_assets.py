# SPDX-License-Identifier: Apache-2.0
"""The build half of web-assets: what gets embedded, and how it is described."""

from __future__ import annotations

import gzip
import importlib.util
import json
import re
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("gen_web_assets", HERE.parent / "gen_web_assets.py")
gen = importlib.util.module_from_spec(spec)
# dataclasses look the defining module up in sys.modules while the class is built.
sys.modules[spec.name] = gen
spec.loader.exec_module(gen)

REPO = HERE.parents[2]
WEB_ASSETS_C = REPO / "modules" / "web-assets" / "lib" / "web_assets.c"

BIG_JS = ("export function f(){return 'cedar';}\n" * 200).encode()


@pytest.fixture
def dist(tmp_path: Path) -> Path:
    d = tmp_path / "dist"
    (d / "assets").mkdir(parents=True)
    (d / "index.html").write_text('<!doctype html><script type="module" src="/assets/index-AbCd1234.js"></script>')
    (d / "assets" / "index-AbCd1234.js").write_bytes(BIG_JS)
    (d / "assets" / "logo-Xy_98765.png").write_bytes(bytes(range(256)) * 8)
    (d / "favicon.svg").write_text("<svg/>")
    (d / ".cedar-build.json").write_text(json.dumps({"version": "1.2.3-test"}))
    (d / ".vite").mkdir()
    (d / ".vite" / "manifest.json").write_text("{}")
    return d


def by_path(assets):
    return {a.path: a for a in assets}


def test_collects_served_files_sorted_and_skips_dotfiles(dist):
    assets = gen.collect(dist)
    paths = [a.path for a in assets]
    assert paths == sorted(paths, key=str.encode)
    assert set(paths) == {"/index.html", "/assets/index-AbCd1234.js",
                          "/assets/logo-Xy_98765.png", "/favicon.svg"}


def test_gzip_decision(dist):
    a = by_path(gen.collect(dist))
    js = a["/assets/index-AbCd1234.js"]
    assert js.gzip and gzip.decompress(js.data) == BIG_JS
    assert not a["/index.html"].gzip, "under 1 KiB stays readable"
    assert not a["/assets/logo-Xy_98765.png"].gzip, "not compressible by type"
    assert not a["/favicon.svg"].gzip


def _noise(n: int) -> bytes:
    """Deterministic bytes with no structure gzip can use."""
    import hashlib
    out, block = b"", b"seed"
    while len(out) < n:
        block = hashlib.sha256(block).digest()
        out += block
    return out[:n]


def test_incompressible_text_stays_identity():
    """A text type gzip cannot shrink by a tenth is stored as it is."""
    data = _noise(4096)
    assert len(gzip.compress(data, 9, mtime=0)) > len(data) * gen.GZIP_MAX_RATIO
    asset = gen.build_asset("/assets/x-ABCDEFGH.txt", "x", data)
    assert not asset.gzip
    assert asset.data == data


def test_the_ratio_threshold_is_the_boundary():
    """Half noise, half zeros: gzip saves about half, well past a tenth."""
    data = _noise(2048) + bytes(2048)
    asset = gen.build_asset("/assets/y-ABCDEFGH.txt", "y", data)
    assert asset.gzip
    assert gzip.decompress(asset.data) == data


def test_mime_types_and_page(dist):
    a = by_path(gen.collect(dist))
    assert a["/index.html"].content_type == "text/html; charset=utf-8"
    assert a["/index.html"].is_page
    assert a["/assets/index-AbCd1234.js"].content_type == "text/javascript; charset=utf-8"
    assert not a["/assets/index-AbCd1234.js"].is_page
    assert a["/favicon.svg"].content_type == "image/svg+xml"


def test_cache_classes(dist):
    a = by_path(gen.collect(dist))
    assert a["/assets/index-AbCd1234.js"].cache == gen.CACHE_IMMUTABLE
    assert a["/assets/logo-Xy_98765.png"].cache == gen.CACHE_IMMUTABLE
    assert a["/index.html"].cache == gen.CACHE_REVALIDATE
    assert a["/favicon.svg"].cache == gen.CACHE_REVALIDATE, "not hashed, not under assets/"
    assert gen.build_asset("/assets/plain.js", "p", b"x").cache == gen.CACHE_REVALIDATE
    assert gen.build_asset("/other/app-AbCd1234.js", "p", b"x").cache == gen.CACHE_REVALIDATE


def test_etag_is_over_stored_bytes_and_changes_with_content(dist):
    a = by_path(gen.collect(dist))
    js = a["/assets/index-AbCd1234.js"]
    assert re.fullmatch(r'"[0-9a-f]{16}"', js.etag)
    import hashlib
    assert js.etag[1:-1] == hashlib.sha256(js.data).hexdigest()[:16]
    other = gen.build_asset("/assets/index-AbCd1234.js", "x", BIG_JS + b"//")
    assert other.etag != js.etag


def test_refusals(dist):
    (dist / "notes.docx").write_bytes(b"x")
    with pytest.raises(gen.AssetError, match="MIME"):
        gen.collect(dist)
    (dist / "notes.docx").unlink()
    (dist / "bad name.js").write_bytes(b"x")
    with pytest.raises(gen.AssetError, match="not a path"):
        gen.collect(dist)
    (dist / "bad name.js").unlink()
    (dist / "Makefile").write_bytes(b"x")
    with pytest.raises(gen.AssetError, match="extension"):
        gen.collect(dist)
    (dist / "Makefile").unlink()
    (dist / "index.html").unlink()
    with pytest.raises(gen.AssetError, match="index.html"):
        gen.collect(dist)


def test_output_is_reproducible(dist, tmp_path):
    out1, out2 = tmp_path / "a.c", tmp_path / "b.c"
    assert gen.main(["--dist", str(dist), "--out-c", str(out1)]) == 0
    assert gen.main(["--dist", str(dist), "--out-c", str(out2)]) == 0
    assert out1.read_bytes() == out2.read_bytes()


def test_unchanged_output_is_not_rewritten(dist, tmp_path):
    out = tmp_path / "a.c"
    assert gen.main(["--dist", str(dist), "--out-c", str(out)]) == 0
    before = out.stat().st_mtime_ns
    assert gen.main(["--dist", str(dist), "--out-c", str(out)]) == 0
    assert out.stat().st_mtime_ns == before


def test_c_table_matches_manifest(dist, tmp_path):
    c_out, m_out = tmp_path / "t.c", tmp_path / "m.json"
    assert gen.main(["--dist", str(dist), "--out-c", str(c_out), "--out-manifest", str(m_out)]) == 0
    c = c_out.read_text()
    manifest = json.loads(m_out.read_text())
    assert manifest["version"] == "1.2.3-test"
    paths_in_c = re.findall(r'\.path = "([^"]+)"', c)
    assert paths_in_c == [a["path"] for a in manifest["assets"]]
    assert f'.count = {len(manifest["assets"])}' in c
    index = [i for i, a in enumerate(manifest["assets"]) if a["is_page"]][0]
    assert f".index = &assets[{index}]" in c
    lens = [int(x) for x in re.findall(r"\.len = (\d+),", c)]
    assert lens == [a["stored_bytes"] for a in manifest["assets"]]


def test_version_override_and_default(dist, tmp_path):
    assert gen.read_version(dist, "override") == "override"
    (dist / ".cedar-build.json").unlink()
    assert gen.read_version(dist, None) == "unknown"


def test_budget(dist, tmp_path):
    assert gen.main(["--dist", str(dist), "--budget-kib", "1"]) == 1
    assert gen.main(["--dist", str(dist), "--budget-kib", "512"]) == 0


def test_csp_literal_is_what_the_mock_will_send():
    """The page's policy lives in C; the mock reads it from there, not a copy."""
    from importlib import util
    text = WEB_ASSETS_C.read_text()
    assert gen.page_csp_from_c(text).startswith("default-src 'self';")
    assert "frame-ancestors 'none'" in gen.page_csp_from_c(text)
