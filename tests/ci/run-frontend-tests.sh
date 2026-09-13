#!/bin/sh
# Run the frontend's checks, in the order a failure is cheapest to read.
#
# Usage:  tests/ci/run-frontend-tests.sh
#
#   1. npm ci          - exactly the lockfile; the build needs no network beyond
#                        the registry (plan section 4)
#   2. npm test        - generates the API types from openapi.json, then the unit
#                        and component tests (vitest)
#   3. npm run build   - typecheck, production build, 512 KiB gzip budget
#   4. playwright      - the end-to-end suite against the mock server, which
#                        serves the built application with the device's static
#                        policy
#
# SKIP_E2E=1 stops after the build. PW_CHANNEL selects the browser: the default
# is the installed Chrome; set it empty to use Playwright's own Chromium, which
# `npx playwright install chromium` downloads. PYTHON names an interpreter with
# jsonschema for the mock (default: the west workspace's, else python3).
set -eu

APP_DIR=$(cd "$(dirname "$0")/../.." && pwd)
cd "$APP_DIR/src/web/frontend"

echo "== npm ci"
npm ci --no-audit --no-fund

echo
echo "== unit and component tests"
npm test

echo
echo "== build"
npm run build

if [ "${SKIP_E2E:-0}" = "1" ]; then
	exit 0
fi

echo
echo "== end-to-end against the mock"
exec npx playwright test
