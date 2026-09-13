import { expect, test } from '@playwright/test';

import { t, watchPage } from './helpers';

// The application as the board serves it (plan section 12: at least one run of
// the user scenarios against the real device before the stage is accepted).
// Runs only with E2E_DEVICE_URL and E2E_DEVICE_PASSWORD set; it signs in with
// an existing administrator and never uses the mock's control plane.
const password = process.env.E2E_DEVICE_PASSWORD ?? '';

test.skip(!process.env.E2E_DEVICE_URL || !password, 'E2E_DEVICE_URL and E2E_DEVICE_PASSWORD are not set');

test('the board serves the application, signs in, shows itself, and signs out', async ({ page, baseURL }) => {
  test.setTimeout(90_000);
  const watch = watchPage(page);
  await page.goto('/');
  await expect(page.locator('html')).toHaveAttribute('lang', 'ru');
  await expect(page.getByRole('heading', { name: t('login.title') })).toBeVisible();
  await expect(page.getByRole('note')).toContainText(t('app.http_warning'));

  await page.getByLabel(t('login.password_label')).fill(password);
  await page.getByRole('button', { name: t('login.submit') }).click();
  await expect(page.getByRole('heading', { name: t('overview.title') })).toBeVisible({ timeout: 30_000 });
  await expect(page.getByText('cedar_switch_3in4out_power')).toBeVisible();
  // This firmware does not serve network, Matter or coprocessor status yet.
  await expect(page.getByText(t('overview.unavailable')).first()).toBeVisible();

  await page.goto('/access');
  await expect(page.getByRole('heading', { name: t('access.title') })).toBeVisible();
  await page.getByRole('banner').getByRole('button', { name: t('nav.logout') }).click();
  await expect(page.getByRole('heading', { name: t('login.title') })).toBeVisible();
  watch.assertStaysHome(baseURL!);
});
