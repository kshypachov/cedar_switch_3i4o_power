import { expect, type Page, test } from '@playwright/test';

import { MOCK_PASSWORD, resetDevice, t, watchPage } from './helpers';

// The mock's install takes about 12 s (eight phases of 1.5 s, writing three);
// a chunk job 0.2 s. Scenario knobs stand in for what the mock cannot observe:
// the transport a request arrived on, a verification outcome, who owns the UART.

async function signIn(page: Page): Promise<void> {
  await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
  await page.getByRole('button', { name: t('login.submit') }).click();
}

async function openScreen(page: Page, scenario: Record<string, unknown> = {}): Promise<void> {
  await resetDevice(page, { setup_required: false, coprocessor_state: 'failed', ...scenario });
  await page.goto('/');
  await signIn(page);
  await page.getByRole('link', { name: t('nav.coprocessor') }).click();
  await expect(page.getByRole('heading', { level: 1, name: t('update.title'), exact: true })).toBeVisible();
  await expect(page.getByTestId('method-uart')).toBeVisible();
}

/** A merged-looking file: an image header at 0x0 and filler; the mock does not parse it. */
function mergedFile(size: number): { name: string; mimeType: string; buffer: Buffer } {
  const buffer = Buffer.alloc(size, 0xff);
  buffer[0] = 0xe9;
  for (let i = 0x10000; i < size; i++) buffer[i] = (i * 13) & 0xff;
  return { name: 'merged-binary.bin', mimeType: 'application/octet-stream', buffer };
}

async function uploadAndVerify(page: Page, size = 70_000): Promise<void> {
  await page.getByLabel(t('update.file_label')).setInputFiles(mergedFile(size));
  await expect(page.getByTestId('file-sha256')).toBeVisible({ timeout: 10_000 });
  await page.getByRole('button', { name: t('update.upload_submit') }).click();
}

async function startInstall(page: Page): Promise<void> {
  await page.getByRole('checkbox', { name: t('update.install_acknowledge') }).check();
  await page.getByRole('button', { name: t('update.install_submit') }).click();
}

test.describe('ESP32 update', () => {
  test('uploads the merged file, verifies it, writes it and follows the phases to a running module', async ({ page, baseURL }) => {
    test.setTimeout(90_000);
    const watch = watchPage(page);
    await openScreen(page);
    await expect(page.getByTestId('coprocessor-state')).toHaveText(t('coprocessor.failed'));
    await expect(page.getByTestId('method-ota')).toContainText(t('update.reason.not_implemented'));

    await uploadAndVerify(page);
    await expect(page.getByTestId('upload-state')).toHaveText(t('update.upload_state.ready'), { timeout: 30_000 });
    await expect(page.getByTestId('image-version')).toHaveText('1');

    await expect(page.getByRole('button', { name: t('update.install_submit') })).toBeDisabled();
    await startInstall(page);
    await expect(page.getByTestId('phase-writing')).toHaveAttribute('data-status', 'current', { timeout: 15_000 });
    await expect(page.getByTestId('coprocessor-uart')).toHaveText(t('logs.uart_mode.flashing'), { timeout: 5_000 });
    await expect(page.getByTestId('install-notice')).toHaveText(t('update.install_succeeded'), { timeout: 30_000 });
    await expect(page.getByTestId('phase-complete')).toHaveAttribute('data-status', 'done');
    await expect(page.getByTestId('coprocessor-version')).toHaveText('v3.0.6', { timeout: 10_000 });
    await expect(page.getByTestId('last-update-state')).toHaveText(t('update.summary.succeeded'));
    watch.assertStaysHome(baseURL!);
  });

  test('a bare application .bin is refused by verification, with the reason', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openScreen(page, { verify_result: 'bare_app' });
    await uploadAndVerify(page, 20_000);
    const failed = page.getByTestId('verify-failed');
    await expect(failed).toContainText(t('error.invalid_image'), { timeout: 30_000 });
    await expect(failed).toContainText('merge-bin');
    await expect(page.getByRole('button', { name: t('update.install_submit') })).toBeDisabled();
    watch.assertStaysHome(baseURL!);
  });

  test('a write requested other than over Ethernet is refused before anything starts', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openScreen(page, { install_transport: 'wifi' });
    await uploadAndVerify(page, 20_000);
    await expect(page.getByTestId('upload-state')).toHaveText(t('update.upload_state.ready'), { timeout: 30_000 });
    await startInstall(page);
    await expect(page.getByTestId('ethernet-required')).toHaveText(t('update.ethernet_required'));
    await expect(page.getByTestId('phase-writing')).toHaveCount(0);
    watch.assertStaysHome(baseURL!);
  });

  test('while the UART is lent to the USB bridge the write is unavailable, and says why', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openScreen(page, { uart_mode: 'usb_bridge' });
    await expect(page.getByTestId('install-unavailable')).toContainText(t('update.reason.uart_usb_bridge'));
    await expect(page.getByTestId('method-uart')).toContainText(t('update.reason.uart_usb_bridge'));
    watch.assertStaysHome(baseURL!);
  });

  test('a reboot in the middle of writing ends as interrupted, recovery required, and nothing continues', async ({ page, baseURL }) => {
    test.setTimeout(90_000);
    const watch = watchPage(page);
    await openScreen(page);
    await uploadAndVerify(page, 20_000);
    await expect(page.getByTestId('upload-state')).toHaveText(t('update.upload_state.ready'), { timeout: 30_000 });
    await startInstall(page);
    await expect(page.getByTestId('phase-writing')).toHaveAttribute('data-status', 'current', { timeout: 15_000 });

    expect((await page.request.post('/__mock/reboot')).ok()).toBe(true);
    await expect(page.getByRole('heading', { name: t('login.title') })).toBeVisible({ timeout: 10_000 });
    await signIn(page);
    await page.getByRole('link', { name: t('nav.coprocessor') }).click();

    await expect(page.getByTestId('last-update-state')).toHaveText(t('update.summary.interrupted'), { timeout: 10_000 });
    await expect(page.getByTestId('recovery-required')).toBeVisible();
    await expect(page.getByTestId('install-notice')).toHaveText(t('update.job_gone'), { timeout: 10_000 });
    // The file survived the reboot and can be written again.
    await expect(page.getByTestId('upload-state')).toHaveText(t('update.upload_state.ready'));
    watch.assertStaysHome(baseURL!);
  });

  test('an upload cut short by a reload continues from the device offset', async ({ page, baseURL }) => {
    test.setTimeout(120_000);
    const watch = watchPage(page);
    await openScreen(page);
    const file = mergedFile(400_000);
    await page.getByLabel(t('update.file_label')).setInputFiles(file);
    await expect(page.getByTestId('file-sha256')).toBeVisible({ timeout: 10_000 });
    await page.getByRole('button', { name: t('update.upload_submit') }).click();
    await expect
      .poll(async () => Number((await page.getByTestId('upload-progress').textContent())?.match(/\d+/)?.[0] ?? 0), {
        timeout: 20_000,
      })
      .toBeGreaterThan(32_000);

    await page.reload();
    await expect(page.getByTestId('upload-state')).toHaveText(t('update.upload_state.receiving'), { timeout: 10_000 });
    const resumedFrom = Number((await page.getByTestId('upload-progress').textContent())?.match(/\d+/)?.[0]);
    expect(resumedFrom).toBeGreaterThan(0);
    await page.getByLabel(t('update.file_label')).setInputFiles(file);
    await page.getByRole('button', { name: t('update.upload_resume') }).click();
    await expect(page.getByTestId('upload-state')).toHaveText(t('update.upload_state.ready'), { timeout: 90_000 });

    // Deleting frees the slot for another file.
    await page.getByRole('button', { name: t('update.delete_submit') }).click();
    await expect(page.getByTestId('upload-state')).toHaveCount(0, { timeout: 10_000 });
    watch.assertStaysHome(baseURL!);
  });
});
