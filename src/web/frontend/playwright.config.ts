import { existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { defineConfig } from '@playwright/test';

const here = dirname(fileURLToPath(import.meta.url));

// End-to-end tests run the built application (dist) against the mock server,
// which serves it on the same origin as the API with the device's static
// policy (tools/api-contract/cedar_contract/mock/static.py). The browser is the
// installed Chrome by default, so running the suite downloads nothing; set
// PW_CHANNEL= (empty) to use Playwright's own Chromium instead.
const PORT = 8099;
const channel = process.env.PW_CHANNEL ?? 'chrome';
// The west workspace's interpreter has jsonschema; fall back to python3 on a
// machine without the workspace (CI installs the two packages itself).
const workspacePython = resolve(here, '../../../../.venv/bin/python');
const python = process.env.PYTHON ?? (existsSync(workspacePython) ? workspacePython : 'python3');
const contractTool = resolve(here, '../../../tools/api-contract');
const dist = resolve(here, 'dist');

// E2E_DEVICE_URL=http://<board address> with E2E_DEVICE_PASSWORD runs e2e/device.spec.ts
// against that board instead of the mock suites against the mock. Check the address
// is the board you mean: two benches share one network (reports/p5/hw).
const device = process.env.E2E_DEVICE_URL;

export default defineConfig({
  testDir: 'e2e',
  testMatch: device ? 'device.spec.ts' : /^(?!device\.).*\.spec\.ts$/,
  fullyParallel: false,
  workers: 1,
  retries: 0,
  reporter: [['list']],
  use: {
    baseURL: device ?? `http://127.0.0.1:${PORT}`,
    ...(channel ? { channel } : {}),
    trace: 'retain-on-failure',
  },
  webServer: device
    ? undefined
    : {
    command: `"${python}" -m cedar_contract.mock --port ${PORT} --ui "${dist}"`,
    cwd: contractTool,
    url: `http://127.0.0.1:${PORT}/api/v1/auth/state`,
    reuseExistingServer: false,
    stdout: 'ignore',
    stderr: 'pipe',
  },
});
