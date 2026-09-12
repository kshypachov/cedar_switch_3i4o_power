# SPDX-License-Identifier: Apache-2.0
"""`python -m cedar_contract.mock --port 8080`."""

import sys

from .server import main

if __name__ == "__main__":
    sys.exit(main())
