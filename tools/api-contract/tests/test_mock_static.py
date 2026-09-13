# SPDX-License-Identifier: Apache-2.0
"""The frontend as the mock serves it: the device's web-assets rules, by case.

The same expectations as tests/web_api_http (Zephyr's server over loopback) and
tests/web_assets checks in C, so the page an e2e test gets from the mock is the
page the browser will get from the device.
"""

from __future__ import annotations

import gzip
import re
from pathlib import Path

import pytest

from cedar_contract.mock.app import MockApp
from cedar_contract.mock.static import WEB_ASSETS_C, StaticSite, accepts_gzip, etag_matches
from cedar_contract.mock.wire import Request

JS = "export const cedar = () => 'device';\n" * 80


@pytest.fixture
def app(tmp_path: Path, document) -> MockApp:
    dist = tmp_path / "dist"
    (dist / "assets").mkdir(parents=True)
    (dist / "index.html").write_text('<!doctype html><div id="root"></div>')
    (dist / "assets" / "app-AbCdEf12.js").write_text(JS)
    (dist / "favicon.svg").write_text("<svg/>")
    app = MockApp(document=document)
    app.static = StaticSite(dist)
    return app


def get(app: MockApp, path: str, **headers: str):
    return app.handle(Request("GET", path, headers))


def test_the_page(app: MockApp) -> None:
    r = get(app, "/")
    assert r.status == 200
    assert r.headers["Content-Type"] == "text/html; charset=utf-8"
    assert r.headers["Cache-Control"] == "no-cache"
    assert r.headers["X-Frame-Options"] == "DENY"
    assert r.headers["Referrer-Policy"] == "no-referrer"
    assert "X-Request-ID" not in r.headers, "static answers are not API answers"
    literal = "".join(re.findall(r'"([^"]*)"', WEB_ASSETS_C.read_text().split("web_assets_page_csp", 1)[1].split(";\n", 1)[0]))
    assert r.headers["Content-Security-Policy"] == literal


def test_navigation_gets_the_page_and_files_do_not(app: MockApp) -> None:
    assert b'id="root"' in get(app, "/access").body
    assert b'id="root"' in get(app, "/network/wifi").body
    assert get(app, "/assets/missing-AbCdEf12.js").status == 404
    assert get(app, "/robots.txt").status == 404
    assert app.handle(Request("POST", "/access")).status == 405


def test_api_paths_are_never_the_page(app: MockApp) -> None:
    for path in ("/api/v1/nope", "/api", "/api/whatever"):
        r = get(app, path)
        assert r.status == 404
        assert r.json["error"]["code"] == "not_found"


def test_gzip_negotiation_and_revalidation(app: MockApp) -> None:
    r = get(app, "/assets/app-AbCdEf12.js", **{"accept-encoding": "gzip, br"})
    assert r.status == 200
    assert r.headers["Content-Encoding"] == "gzip"
    assert r.headers["Vary"] == "Accept-Encoding"
    assert r.headers["Cache-Control"] == "public, max-age=31536000, immutable"
    assert gzip.decompress(r.body).decode() == JS

    assert get(app, "/assets/app-AbCdEf12.js").status == 406

    etag = r.headers["ETag"]
    again = get(app, "/assets/app-AbCdEf12.js", **{"accept-encoding": "gzip", "if-none-match": f"W/{etag}"})
    assert again.status == 304
    assert again.headers["Connection"] == "close"
    assert again.body == b""


@pytest.mark.parametrize(
    ("value", "accepted"),
    [
        (None, False),
        ("gzip", True),
        ("GZIP", True),
        ("x-gzip", True),
        ("deflate, br", False),
        ("*", True),
        ("gzip;q=0", False),
        ("gzip; q=0.000", False),
        ("gzip;q=0.5", True),
        ("*;q=0, gzip", True),
        ("gzip;q=0, *", False),
        ("br, *;q=0", False),
    ],
)
def test_accepts_gzip(value, accepted) -> None:
    assert accepts_gzip(value) is accepted


@pytest.mark.parametrize(
    ("value", "match"),
    [
        (None, False),
        ('"abc"', True),
        ('W/"abc"', True),
        ('"x", "abc"', True),
        ("*", True),
        ('"abd"', False),
        ("abc", False),
    ],
)
def test_etag_matches(value, match) -> None:
    assert etag_matches(value, '"abc"') is match
