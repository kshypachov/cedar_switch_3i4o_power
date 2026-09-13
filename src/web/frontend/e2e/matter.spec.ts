import { expect, test } from '@playwright/test';

import { MOCK_PASSWORD, resetDevice, t, watchPage } from './helpers';

test.describe('matter', () => {
  test.beforeEach(async ({ page }) => {
    await resetDevice(page, { setup_required: false });
  });

  test('opens a window, shows its codes, closes it, and lists the fabric it left', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await page.goto('/');
    await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
    await page.getByRole('button', { name: t('login.submit') }).click();
    await page.getByRole('link', { name: t('nav.matter') }).click();

    await expect(page.getByRole('heading', { level: 1, name: t('matter.title'), exact: true })).toBeVisible();
    await expect(page.getByText(t('matter.fabrics_empty'))).toBeVisible();
    await page.getByLabel(t('matter.timeout_label')).selectOption('300');
    await page.getByRole('button', { name: t('matter.open_submit') }).click();

    // The mock's fixed codes (not a device's): three values, shown apart.
    await expect(page.getByTestId('manual-code')).toHaveText('34970112332', { timeout: 10_000 });
    await expect(page.getByTestId('setup-passcode')).toHaveText('20202021');
    await expect(page.getByRole('img', { name: t('matter.qr_label') })).toBeVisible();

    await page.getByRole('button', { name: t('matter.close_submit') }).click();
    await expect(page.getByText(t('matter.codes.window_closed'))).toBeVisible({ timeout: 10_000 });
    await expect(page.getByRole('table')).toBeVisible({ timeout: 10_000 });

    // A reload lands on the same screen.
    await page.reload();
    await expect(page.getByRole('heading', { level: 1, name: t('matter.title'), exact: true })).toBeVisible();
    watch.assertStaysHome(baseURL!);
    expect(watch.problems).toEqual([]);
  });
});
