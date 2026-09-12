#!/bin/sh
# Run the contract checks and the mock's test suite.
#
# Usage:  tests/ci/run-contract-tests.sh [extra pytest args...]
#
# One command for both halves of section 12's contract work, because CI runs
# them as one step and a developer should not have to remember two:
#
#   1. the checks on openapi.json itself — internal $ref, required path
#      parameters, unique operationId, valid examples
#   2. the mock server's suite, which is also the contract test: every response
#      it produces is validated against the schema the document declares
#
# No container, unlike the sim tier: this is plain Python with no Zephyr in it.
# The only dependencies are jsonschema and pytest, both of which the toolchain
# already installs (zephyr/scripts/requirements-base.txt and tests/ci/Dockerfile).
set -eu

APP_DIR=$(cd "$(dirname "$0")/../.." && pwd)
TOOL_DIR="$APP_DIR/tools/api-contract"

# Prefer the workspace venv, which is where west put the toolchain. Falling back
# to python3 keeps this working on a runner with no west workspace, which is the
# case for the fast half of the GitHub workflow.
if [ -x "$APP_DIR/../.venv/bin/python" ]; then
	PYTHON="$APP_DIR/../.venv/bin/python"
else
	PYTHON=${PYTHON:-python3}
fi

if ! "$PYTHON" -c 'import jsonschema, pytest' 2>/dev/null; then
	echo "error: $PYTHON has no jsonschema or no pytest." >&2
	echo "       Both are toolchain dependencies; install them with" >&2
	echo "         $PYTHON -m pip install jsonschema pytest" >&2
	exit 1
fi

cd "$TOOL_DIR"

echo "== contract checks on docs/device-development/openapi.json"
"$PYTHON" -m cedar_contract.checks

echo
echo "== mock server and contract test suite"
exec "$PYTHON" -m pytest "$@"
