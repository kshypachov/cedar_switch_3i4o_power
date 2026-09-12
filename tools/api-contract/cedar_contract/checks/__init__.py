# SPDX-License-Identifier: Apache-2.0
"""Checks section 12 of the development plan requires on `openapi.json`.

The plan lists five. Four are implemented here; the fifth is recorded as
deferred with its reason in `document.DEFERRED`, because a check that cannot
be performed is better named than quietly missing.

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
    run_all,
)
