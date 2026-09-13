# SPDX-License-Identifier: Apache-2.0
"""The browser application, served the way the device serves it.

`python -m cedar_contract.mock --ui <dist>` puts a built frontend on the same
origin as the mock API, so the frontend's end-to-end tests run against the page
they will get from the device: the same files, the same headers, the same
refusals. None of that is decided here a second time:

- which files, their MIME types, gzip or identity, ETags and cache classes come
  from tools/web-assets/gen_web_assets.py, the script the firmware build runs;
- the page's Content-Security-Policy is read out of
  modules/web-assets/lib/web_assets.c;
- the rest - GET only, SPA fallback for navigation, 406 for a gzip-only file,
  304 with Connection: close - is `web_assets_respond()` transcribed, and
  tests/test_mock_static.py checks the transcription case by case against the
  same expectations as the C suite.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
from types import ModuleType

from .wire import Request, Response

REPO = Path(__file__).resolve().parents[4]
GENERATOR = REPO / "tools" / "web-assets" / "gen_web_assets.py"
WEB_ASSETS_C = REPO / "modules" / "web-assets" / "lib" / "web_assets.c"

CACHE_CONTROL = {
    "revalidate": "no-cache",
    "immutable": "public, max-age=31536000, immutable",
}


def generator() -> ModuleType:
    spec = importlib.util.spec_from_file_location("gen_web_assets", GENERATOR)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _trim(text: str) -> str:
    return text.strip(" \t")


def accepts_gzip(accept_encoding: str | None) -> bool:
    """web_assets_accepts_gzip(): explicit gzip wins over *, q=0 refuses, absent is no."""
    if accept_encoding is None:
        return False
    gzip = star = None
    for item in accept_encoding.split(","):
        token, _, params = item.partition(";")
        zero = False
        for param in params.split(";"):
            name, eq, value = _trim(param).partition("=")
            if eq and name.lower() == "q":
                zero = bool(value) and value[0] == "0" and all(c in ".0" for c in value[1:])
                break
        token = _trim(token).lower()
        if token in ("gzip", "x-gzip"):
            gzip = not zero
        elif token == "*":
            star = not zero
    if gzip is not None:
        return gzip
    return bool(star)


def etag_matches(if_none_match: str | None, etag: str) -> bool:
    """web_assets_etag_matches(): weak comparison, * matches anything."""
    if if_none_match is None:
        return False
    strip = lambda t: t[2:] if t.startswith("W/") else t  # noqa: E731
    wanted = strip(etag)
    for item in if_none_match.split(","):
        item = _trim(item)
        if item == "*" or strip(item) == wanted:
            return True
    return False


def _is_navigation(path: str) -> bool:
    if path.startswith("/assets/"):
        return False
    return "." not in path.rsplit("/", 1)[-1]


def _plain(status: int, text: str) -> Response:
    return Response(
        status,
        text.encode(),
        {
            "Content-Type": "text/plain; charset=utf-8",
            "Cache-Control": "no-store",
            "X-Content-Type-Options": "nosniff",
        },
    )


class StaticSite:
    """A dist directory, answered by the device's rules."""

    def __init__(self, dist: Path) -> None:
        gen = generator()
        self.assets = {a.path: a for a in gen.collect(Path(dist))}
        self.index = self.assets["/index.html"]
        self.csp = gen.page_csp_from_c(WEB_ASSETS_C.read_text())

    def respond(self, request: Request) -> Response:
        if request.method != "GET":
            return _plain(405, "Method not allowed\n")
        path = request.path
        asset = self.index if path == "/" else self.assets.get(path)
        if asset is None and _is_navigation(path):
            asset = self.index
        if asset is None:
            return _plain(404, "Not found\n")
        if asset.gzip and not accepts_gzip(request.headers.get("accept-encoding")):
            response = _plain(406, "This file is only available gzip-encoded\n")
            response.headers["Vary"] = "Accept-Encoding"
            return response

        headers = {"ETag": asset.etag, "Cache-Control": CACHE_CONTROL[asset.cache]}
        if asset.gzip:
            headers["Vary"] = "Accept-Encoding"
        if etag_matches(request.headers.get("if-none-match"), asset.etag):
            headers["Connection"] = "close"
            return Response(304, b"", headers)
        headers["Content-Type"] = asset.content_type
        headers["X-Content-Type-Options"] = "nosniff"
        if asset.gzip:
            headers["Content-Encoding"] = "gzip"
        if asset.is_page:
            headers["Content-Security-Policy"] = self.csp
            headers["Referrer-Policy"] = "no-referrer"
            headers["X-Frame-Options"] = "DENY"
        return Response(200, asset.data, headers)
