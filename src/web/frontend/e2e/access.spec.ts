import { expect, test } from '@playwright/test';

import { MOCK_PASSWORD, resetDevice, SETUP_TOKEN, t, watchPage } from './helpers';

test.describe('a fresh device', () => {
  test.beforeEach(async ({ page }) => {
    await resetDevice(page);
  });

  test('is set up from the page, in Russian, with the HTTP warning in view', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await page.goto('/');
    await expect(page.locator('html')).toHaveAttribute('lang', 'ru');
    await expect(page.getByRole('heading', { name: t('setup.title') })).toBeVisible();
    await expect(page.getByTestId('setup-token')).toHaveText(SETUP_TOKEN);
    await expect(page.getByRole('note')).toContainText(t('app.http_warning'));

    await page.getByLabel(t('setup.password_label')).fill('a good long password');
    await page.getByLabel(t('setup.password_confirm_label')).fill('a good long password');
    await page.getByRole('button', { name: t('setup.submit') }).click();

    await expect(page.getByRole('heading', { name: t('overview.title') })).toBeVisible();
    await expect(page.getByText('cedar_switch_3in4out_power')).toBeVisible();
    watch.assertStaysHome(baseURL!);
  });

  test('refuses a short password before asking the device', async ({ page }) => {
    const watch = watchPage(page);
    await page.goto('/');
    await page.getByLabel(t('setup.password_label')).fill('short');
    await page.getByLabel(t('setup.password_confirm_label')).fill('short');
    await page.getByRole('button', { name: t('setup.submit') }).click();
    await expect(page.getByText('Не короче 12 символов.')).toBeVisible();
    expect(watch.requests.filter((r) => r.method() === 'POST')).toHaveLength(0);
  });
});

test.describe('a configured device', () => {
  test.beforeEach(async ({ page }) => {
    await resetDevice(page, { setup_required: false });
  });

  test('signs in, survives a reload, and signs out', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await page.goto('/');
    await page.getByLabel(t('login.password_label')).fill('not the password');
    await page.getByRole('button', { name: t('login.submit') }).click();
    await expect(page.getByRole('alert')).toContainText(t('error.invalid_credentials'));

    await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
    await page.getByRole('button', { name: t('login.submit') }).click();
    await expect(page.getByRole('heading', { name: t('overview.title') })).toBeVisible();

    const cookies = await page.context().cookies();
    const session = cookies.find((c) => c.name === 'cedar_session');
    expect(session?.httpOnly).toBe(true);
    expect(session?.sameSite).toBe('Strict');
    expect(session?.secure).toBe(false);

    await page.goto('/access');
    await expect(page.getByRole('heading', { name: t('access.title') })).toBeVisible();

    await page.getByRole('banner').getByRole('button', { name: t('nav.logout') }).click();
    await expect(page.getByRole('heading', { name: t('login.title') })).toBeVisible();
    await page.reload();
    await expect(page.getByRole('heading', { name: t('login.title') })).toBeVisible();
    watch.assertStaysHome(baseURL!);
  });

  test('changes the password and asks for the new one', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await page.goto('/');
    await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
    await page.getByRole('button', { name: t('login.submit') }).click();
    await page.getByRole('link', { name: t('nav.access') }).click();

    await page.getByLabel(t('access.current_password_label')).fill(MOCK_PASSWORD);
    await page.getByLabel(t('access.new_password_label'), { exact: true }).fill('a brand new password');
    await page.getByLabel(t('access.new_password_confirm_label')).fill('a brand new password');
    await page.getByRole('button', { name: t('access.change_submit') }).click();

    await expect(page.getByText(t('login.password_changed'))).toBeVisible({ timeout: 15_000 });
    await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
    await page.getByRole('button', { name: t('login.submit') }).click();
    await expect(page.getByRole('alert')).toContainText(t('error.invalid_credentials'));
    await page.getByLabel(t('login.password_label')).fill('a brand new password');
    await page.getByRole('button', { name: t('login.submit') }).click();
    // Signed in again on the screen the address still names.
    await expect(page).toHaveURL(/\/access$/);
    await expect(page.getByRole('heading', { name: t('access.title') })).toBeVisible();
    watch.assertStaysHome(baseURL!);
  });

  test('two browsers are signed in at once', async ({ browser, baseURL }) => {
    const first = await browser.newContext({ baseURL });
    const second = await browser.newContext({ baseURL });
    const pages = [await first.newPage(), await second.newPage()];
    for (const page of pages) {
      await page.goto('/');
      await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
      await page.getByRole('button', { name: t('login.submit') }).click();
    }
    for (const page of pages) {
      await expect(page.getByRole('heading', { name: t('overview.title') })).toBeVisible();
      await expect(page.getByText('cedar_switch_3in4out_power')).toBeVisible();
    }
    await first.close();
    await second.close();
  });
});
