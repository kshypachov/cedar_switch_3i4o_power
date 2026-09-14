import { expect, type Page, test } from '@playwright/test';

import { MOCK_PASSWORD, resetDevice, t, watchPage } from './helpers';

async function signIn(page: Page): Promise<void> {
  await page.goto('/');
  await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
  await page.getByRole('button', { name: t('login.submit') }).click();
}

async function openLogs(page: Page, scenario: Record<string, unknown> = {}): Promise<void> {
  await resetDevice(page, { setup_required: false, ...scenario });
  await signIn(page);
  await page.getByRole('link', { name: t('nav.logs') }).click();
  await expect(page.getByRole('heading', { level: 1, name: t('logs.title'), exact: true })).toBeVisible();
  await expect(page.getByTestId('log-row').first()).toBeVisible({ timeout: 10_000 });
}

test.describe('logs', () => {
  test('tails, filters by source, pauses, and exports what the filters select', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openLogs(page);

    await page.getByLabel(t('logs.source_label'), { exact: true }).selectOption('esp32');
    await expect
      .poll(async () => {
        const sources = await page.getByTestId('log-row').evaluateAll((rows) => rows.map((r) => r.getAttribute('data-source')));
        return sources.length > 0 && sources.every((s) => s === 'esp32');
      }, { timeout: 10_000 })
      .toBe(true);

    await page.getByRole('button', { name: t('logs.pause') }).click();
    await expect(page.getByText(t('logs.paused_notice'))).toBeVisible();
    await page.getByRole('button', { name: t('logs.resume') }).click();
    await expect(page.getByText(t('logs.paused_notice'))).toBeHidden();

    const href = await page.getByTestId('log-export-ndjson').getAttribute('href');
    expect(href).toContain('source=esp32');
    const response = await page.request.get(href!);
    expect(response.status()).toBe(200);
    expect(response.headers()['content-type']).toContain('application/x-ndjson');
    expect(response.headers()['content-disposition']).toContain('attachment');
    const lines = (await response.text()).trim().split('\n').map((line) => JSON.parse(line) as { source: string; kind: string });
    expect(lines.length).toBeGreaterThan(0);
    expect(lines.filter((r) => r.kind !== 'gap').every((r) => r.source === 'esp32')).toBe(true);

    const text = await page.request.get((await page.getByTestId('log-export-text').getAttribute('href'))!);
    expect(text.headers()['content-type']).toContain('text/plain');
    expect(text.headers()['content-disposition']).toContain('attachment');

    watch.assertStaysHome(baseURL!);
  });

  test('a pause long enough for the ring to move on ends in a gap', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openLogs(page, { log_ring_records: 50 });
    await page.getByRole('button', { name: t('logs.pause') }).click();
    await expect(page.getByText(t('logs.paused_notice'))).toBeVisible();

    // While the page sends nothing, the device logs far more than its ring keeps.
    await page.request.post('/__mock/scenario', { data: { log_rate_per_s: 100 } });
    expect((await page.request.post('/__mock/advance', { data: { seconds: 10 } })).ok()).toBe(true);
    await page.request.post('/__mock/scenario', { data: { log_rate_per_s: 0 } });
    await expect(page.getByTestId('log-gap')).toHaveCount(0);

    await page.getByRole('button', { name: t('logs.resume') }).click();
    await expect(page.getByTestId('log-gap').first()).toBeVisible({ timeout: 10_000 });
    watch.assertStaysHome(baseURL!);
  });

  test('says why ESP32 logs stop while the UART is lent to the USB bridge', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openLogs(page, { uart_mode: 'usb_bridge' });
    await expect(page.getByTestId('log-source-esp32')).toContainText(t('logs.reason.uart_usb_bridge'), { timeout: 10_000 });
    await expect(page.getByText(t('logs.uart_mode.usb_bridge'), { exact: true })).toBeVisible();
    await expect(page.getByTestId('log-source-stm32')).toContainText(t('logs.available'));
    watch.assertStaysHome(baseURL!);
  });

  test('a reboot signs the browser out, and the logs after it belong to the new boot', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openLogs(page);
    const before = await page.getByTestId('log-boot-id').textContent();

    expect((await page.request.post('/__mock/reboot')).ok()).toBe(true);
    await expect(page.getByRole('heading', { name: t('login.title') })).toBeVisible({ timeout: 10_000 });

    await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
    await page.getByRole('button', { name: t('login.submit') }).click();
    await page.getByRole('link', { name: t('nav.logs') }).click();
    await expect(page.getByTestId('log-boot-id')).not.toHaveText(before!, { timeout: 10_000 });
    await expect(page.getByTestId('log-boot-id')).toHaveText(/^boot_/);
    watch.assertStaysHome(baseURL!);
  });

  test('marks a new boot and a gap where they happened in the live tail', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openLogs(page);
    // A reboot of the mock also ends the session, so a page can never carry its
    // cursor across one there. The device's answer to that cursor is written
    // here in the contract's shape: the new tail, the new boot_id, gap=true.
    await page.route('**/api/v1/logs/records?*', async (route) => {
      if (!new URL(route.request().url()).searchParams.has('cursor')) return route.continue();
      return route.fulfill({
        status: 200,
        contentType: 'application/json',
        headers: { 'Cache-Control': 'no-store' },
        body: JSON.stringify({
          boot_id: 'boot_e2e_rebooted',
          items: [
            {
              source: 'stm32',
              boot_id: 'boot_e2e_rebooted',
              source_generation: 0,
              seq: '1',
              uptime_ms: '3',
              wall_time: null,
              level: null,
              module: null,
              message: 'first line after the reboot',
              truncated: false,
              kind: 'message',
            },
          ],
          next_cursor: 'e2e-after-reboot',
          has_more: false,
          gap: true,
          dropped_count: '0',
        }),
      });
    });
    await expect(page.getByTestId('log-boot')).toContainText('boot_e2e_rebooted', { timeout: 10_000 });
    await expect(page.getByTestId('log-gap').first()).toBeVisible();
    await expect(page.getByText('first line after the reboot')).toBeVisible();
    await expect(page.getByTestId('log-boot-id')).toHaveText('boot_e2e_rebooted');
    watch.assertStaysHome(baseURL!);
  });
});
