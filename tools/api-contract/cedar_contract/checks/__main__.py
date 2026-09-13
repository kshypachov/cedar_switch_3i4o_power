# SPDX-License-Identifier: Apache-2.0
"""`python -m cedar_contract.checks [path/to/openapi.json]`.

Exits non-zero on the first document that has findings, so it can be a CI step
on its own without a test runner.
"""

from __future__ import annotations

import argparse
import sys

from ..openapi import Document
from .document import CHECKS, DEFERRED, served_operation_ids


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="cedar_contract.checks", description=__doc__)
    parser.add_argument("document", nargs="?", default=None, help="openapi.json to check")
    parser.add_argument("-q", "--quiet", action="store_true", help="print only failures")
    args = parser.parse_args(argv)

    doc = Document.load(args.document)
    total = 0
    for name, check in CHECKS.items():
        findings = check(doc)
        total += len(findings)
        if findings:
            print(f"FAIL {name}: {len(findings)} finding(s)")
            for finding in findings:
                print(f"  {finding.pointer}: {finding.message}")
        elif not args.quiet:
            print(f"ok   {name}")

    if not args.quiet:
        for name, reason in DEFERRED.items():
            print(f"skip {name}: {reason}")
        served = served_operation_ids()
        pending = [op.operation_id for op in doc.operations if op.operation_id not in served]
        print(
            f"info the device serves {len(served)} of {len(doc.operations)} operations; "
            f"not yet: {', '.join(pending) if pending else 'none'}"
        )
        print(
            f"\n{len(doc.operations)} operations, {len(doc.schemas)} schemas, "
            f"{len(list(doc.iter_refs()))} references, "
            f"{len(list(doc.iter_examples()))} examples checked"
        )

    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
