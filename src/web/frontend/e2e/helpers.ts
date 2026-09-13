import { expect, type Page, type Request } from '@playwright/test';

import { ru } from '../src/i18n/ru';

export const t = (key: keyof typeof ru): string => ru[key];

export const SETUP_TOKEN = 'cedar-mock-setup-token';
export const MOCK_PASSWORD = 'cedar-mock-admin';

/** Reset the mock between tests. `/__mock` is outside /api/v1, which the
 *  application itself never calls - a test that finds it in the page's
 *  requests fails (see watchRequests). */
export async function resetDevice(page: Page, scenario: Record<string, unknown> = {}): Promise<void> {
  const response = await page.request.post('/__mock/reset', { data: { scenario } });
  expect(response.ok()).toBe(true);
}

/**
 * Everything the page asks for, and every CSP report, so a test can assert the
 * application stays on its own origin (plan section 4: no CDN, no external
 * requests) and never trips its own Content-Security-Policy.
 */
export function watchPage(page: Page) {
  const requests: Request[] = [];
  const problems: string[] = [];
  page.on('request', (r) => requests.push(r));
  page.on('console', (m) => {
    if (m.type() === 'error' && !/Failed to load resource: the server responded with a status of (401|403|404|409|422|429)/.test(m.text())) {
      problems.push(m.text());
    }
  });
  page.on('pageerror', (e) => problems.push(String(e)));
  return {
    requests,
    problems,
    assertStaysHome(origin: string) {
      const foreign = requests.map((r) => r.url()).filter((u) => !u.startsWith(origin));
      expect(foreign, 'requests leaving the origin').toEqual([]);
      const control = requests.map((r) => r.url()).filter((u) => u.includes('/__mock'));
      expect(control, 'the application must not depend on the mock control plane').toEqual([]);
      expect(problems, 'console errors, CSP violations included').toEqual([]);
    },
  };
}
