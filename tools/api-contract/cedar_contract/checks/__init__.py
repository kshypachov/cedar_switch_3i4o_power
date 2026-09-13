# SPDX-License-Identifier: Apache-2.0
"""Checks section 12 of the development plan requires on `openapi.json`.

The plan lists five, and all five run. The fifth - no route the device
serves is missing from the document - waited in `document.DEFERRED` until P2
gave the device a route table to compare (src/web/api/v1/routes.h).

Two of the four already passed when they were written, by hand, on
2026-09-12. That is not an argument against automating them — the value of a
check is that it fails the day someone adds an operation with a duplicated
`operationId`, and a check nobody runs has no such day.
"""

from .document import (  # noqa: F401
    CHECKS,
    DEFERRED,
    Finding,
    check_examples_valid,
    check_operation_ids,
    check_path_parameters,
    check_refs_resolve,
    check_undocumented_routes,
    parse_routes,
    run_all,
    served_operation_ids,
)
