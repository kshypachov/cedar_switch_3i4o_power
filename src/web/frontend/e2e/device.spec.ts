import { expect, test } from '@playwright/test';

import { t, watchPage } from './helpers';

// The application as the board serves it (plan section 12: at least one run of
// the user scenarios against the real device before the stage is accepted).
// Runs only with E2E_DEVICE_URL and E2E_DEVICE_PASSWORD set; it signs in with
// an existing administrator and never uses the mock's control plane.
const password = process.env.E2E_DEVICE_PASSWORD ?? '';

test.skip(!process.env.E2E_DEVICE_URL || !password, 'E2E_DEVICE_URL and E2E_DEVICE_PASSWORD are not set');

test('the board serves the application, signs in, shows itself, Matter and the network, and signs out', async ({ page, baseURL }) => {
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
  // Coprocessor status is served since P5; before that the card says it is not.
  await expect(page.getByRole('region', { name: t('overview.coprocessor') })).toBeVisible();

  // Matter is served since P3. Read-only here: opening a window on the board
  // is a hardware check with a controller, not part of this run.
  await page.getByRole('link', { name: t('nav.matter') }).click();
  await expect(page.getByRole('heading', { level: 1, name: t('matter.title'), exact: true })).toBeVisible();
  await expect(page.getByText(t('matter.ready')).first()).toBeVisible({ timeout: 30_000 });
  await expect(page.getByRole('button', { name: t('matter.open_submit') }).or(page.getByRole('button', { name: t('matter.close_submit') }))).toBeVisible();

  // The network is served since P4. Read-only here: a change on the board
  // moves its address, which reports/p4/hw drives outside the browser. The
  // address this run reached the board by is one the screen shows.
  await page.getByRole('link', { name: t('nav.network') }).click();
  await expect(page.getByRole('heading', { level: 1, name: t('network.title'), exact: true })).toBeVisible();
  await expect(page.getByText(t('network.dns_in_force')).first()).toBeVisible({ timeout: 30_000 });
  await expect(page.getByText(new URL(baseURL!).hostname).first()).toBeVisible();

  // Logs are served since P5. Reading only: the tail and the sources. A
  // firmware before P5 answers 404, which the screen says in words.
  await page.getByRole('link', { name: t('nav.logs') }).click();
  await expect(page.getByRole('heading', { level: 1, name: t('logs.title'), exact: true })).toBeVisible();
  await expect(page.getByTestId('log-row').first().or(page.getByText(t('overview.unavailable')).first())).toBeVisible({ timeout: 30_000 });

  await page.goto('/access');
  await expect(page.getByRole('heading', { name: t('access.title') })).toBeVisible();
  await page.getByRole('banner').getByRole('button', { name: t('nav.logout') }).click();
  await expect(page.getByRole('heading', { name: t('login.title') })).toBeVisible();
  watch.assertStaysHome(baseURL!);
});
