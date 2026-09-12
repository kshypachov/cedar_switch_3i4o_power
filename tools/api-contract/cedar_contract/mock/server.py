# SPDX-License-Identifier: Apache-2.0
"""The only part of the mock that knows about HTTP.

Deliberately `http.server` from the standard library. The mock's reason to exist
is that the frontend can talk to it over HTTP; nothing about that needs a
framework, and adding one would put a dependency into a repository whose Python
requirement is currently "whatever the toolchain already installs".

Single-threaded on purpose. The state machines are not locked, and the contract
tells the client to poll one request at a time and to finish a chunk before
sending the next, so serialising requests matches the device's own behaviour
better than a thread pool would. Static assets are not served: this is the API,
and P2 will run its own dev server in front of it.
"""

from __future__ import annotations

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Any

from ..openapi import Document
from .app import MockApp
from .scenario import Scenario
from .wire import Request

#: Requests larger than this are refused before being read into memory. The API
#: never needs more: the contract caps a JSON body at 8 KiB and a chunk at 16.
MAX_REQUEST_BYTES = 1 << 20


class _Handler(BaseHTTPRequestHandler):
    server_version = "cedar-mock/1"
    protocol_version = "HTTP/1.1"

    app: MockApp

    def do_GET(self) -> None:  # noqa: N802 - http.server's naming
        self._serve("GET")

    def do_POST(self) -> None:  # noqa: N802
        self._serve("POST")

    def do_PUT(self) -> None:  # noqa: N802
        self._serve("PUT")

    def do_DELETE(self) -> None:  # noqa: N802
        self._serve("DELETE")

    def do_OPTIONS(self) -> None:  # noqa: N802
        # Same origin in production, so CORS is not modelled; an OPTIONS answer
        # that allowed cross-origin requests would let the frontend be developed
        # against a permission the device does not grant.
        self.send_response(204)
        self.send_header("Allow", "GET, POST, PUT, DELETE, OPTIONS")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _serve(self, method: str) -> None:
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_REQUEST_BYTES:
            self.send_response(413)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body = self.rfile.read(length) if length else b""
        response = self.app.handle(
            Request(
                method=method,
                path=self.path,
                headers={k: v for k, v in self.headers.items()},
                body=body,
            )
        )
        self.send_response(response.status)
        for name, value in response.headers.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(response.body)))
        self.end_headers()
        if response.body:
            self.wfile.write(response.body)

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stderr.write(f"{self.command} {self.path} -> {fmt % args}\n")


def serve(
    host: str = "127.0.0.1",
    port: int = 8080,
    document: Document | None = None,
    scenario: Scenario | None = None,
) -> None:
    app = MockApp(document=document, scenario=scenario)
    handler = type("_BoundHandler", (_Handler,), {"app": app})
    httpd = HTTPServer((host, port), handler)
    print(
        f"cedar mock API on http://{host}:{port}{app.doc.base_path} "
        f"({len(app.doc.operations)} operations); control plane at /__mock",
        file=sys.stderr,
    )
    if app.state.auth.setup_required:
        print(
            f"  fresh device: POST {app.doc.base_path}/auth/setup with "
            f"X-Setup-Token: {app.state.scenario.setup_token}",
            file=sys.stderr,
        )
    else:
        print(f"  admin password: {app.state.scenario.admin_password}", file=sys.stderr)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


def _parse_scenario(values: list[str]) -> Scenario:
    scenario = Scenario()
    payload: dict[str, Any] = {}
    for item in values:
        key, _, raw = item.partition("=")
        try:
            payload[key] = json.loads(raw)
        except ValueError:
            payload[key] = raw
    scenario.update(payload)
    return scenario


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="cedar_contract.mock", description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--document", default=None, help="openapi.json to route from")
    parser.add_argument(
        "--scenario",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="scenario knob, repeatable (e.g. setup_required=false wifi_scan=truncated)",
    )
    args = parser.parse_args(argv)
    serve(
        host=args.host,
        port=args.port,
        document=Document.load(args.document) if args.document else None,
        scenario=_parse_scenario(args.scenario),
    )
    return 0
