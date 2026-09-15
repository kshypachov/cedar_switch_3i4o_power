# SPDX-License-Identifier: Apache-2.0
"""Tooling that holds the web API contract to its own document.

Two deliverables of stage P1 live here, and they share a package because they
share the same two facts: what `openapi.json` says, and what the device's
rejection looks like.

- `cedar_contract.mock` — a server that answers all thirty-seven operations so
  the frontend of stage P2 can be written and tested before the device has a
  single HTTP handler.
- `cedar_contract.checks` — the checks section 12 of the development plan
  requires on the document itself.

See README.md in this directory for the decisions behind both.
"""

__all__ = ["errors", "openapi"]
