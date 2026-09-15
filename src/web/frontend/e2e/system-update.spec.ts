import { expect, type Page, test } from '@playwright/test';

import { MOCK_PASSWORD, resetDevice, t, watchPage } from './helpers';
import { mcubootImage, type Version, versionText } from './mcuboot';

// The mock's STM32 install (tools/api-contract/cedar_contract/mock/system.py):
// preparing and requesting 1.5 s each, rebooting 2 s, then the device restarts,
// answers nothing for system_swap_ms while MCUboot swaps, and comes back with a
// new boot and no sessions. The new firmware confirms itself after
// system_confirm_seconds; POST /__mock/reboot before that reverts it.

const NEW: Version = { major: 1, minor: 1, revision: 0, build: 0 };

async function signIn(page: Page): Promise<void> {
  await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
  await page.getByRole('button', { name: t('login.submit') }).click();
}

async function openScreen(page: Page, scenario: Record<string, unknown> = {}): Promise<void> {
  await resetDevice(page, { setup_required: false, ...scenario });
  await page.goto('/');
  await signIn(page);
  await page.getByRole('link', { name: t('nav.firmware') }).click();
  await expect(page.getByRole('heading', { level: 1, name: t('sysupd.title'), exact: true })).toBeVisible();
  await expect(page.getByTestId('running-version')).toBeVisible();
}

/**
 * The restart is the point of these tests: while MCUboot swaps, the device
 * answers nothing, and Chrome logs each poll it could not complete. Those - and
 * only those - are expected; every other console error and CSP report still fails.
 */
function expectRestartSilence(watch: ReturnType<typeof watchPage>): void {
  const silence = /^Failed to load resource: net::ERR_(EMPTY_RESPONSE|CONNECTION_REFUSED|CONNECTION_RESET|CONNECTION_CLOSED)$/;
  const excused = watch.problems.filter((p) => silence.test(p));
  expect(excused.length, 'the device was silent at least once during the restart').toBeGreaterThan(0);
  const rest = watch.problems.filter((p) => !silence.test(p));
  watch.problems.splice(0, watch.problems.length, ...rest);
}

function file(buffer: Buffer, name = 'zephyr.signed.bin') {
  return { name, mimeType: 'application/octet-stream', buffer };
}

async function uploadAndVerify(page: Page, buffer: Buffer): Promise<void> {
  await page.getByLabel(t('sysupd.file_label')).setInputFiles(file(buffer));
  await expect(page.getByTestId('system-file-sha256')).toBeVisible({ timeout: 10_000 });
  await page.getByRole('button', { name: t('update.upload_submit') }).click();
}

async function install(page: Page): Promise<void> {
  await page.getByRole('button', { name: t('sysupd.install_submit') }).click();
  await expect(page.getByTestId('system-confirm')).toBeVisible();
  await page.getByRole('button', { name: t('sysupd.confirm_submit') }).click();
}

/** The restart ends the session: sign in again, and the path is still /firmware. */
async function signInAfterRestart(page: Page): Promise<void> {
  await expect(page.getByText(t('login.session_ended'))).toBeVisible({ timeout: 40_000 });
  await signIn(page);
  await expect(page.getByRole('heading', { level: 1, name: t('sysupd.title'), exact: true })).toBeVisible();
}

test.describe('STM32 update', () => {
  test('uploads, verifies, installs through the restart, and the new firmware confirms itself', async ({ page, baseURL }) => {
    test.setTimeout(120_000);
    const watch = watchPage(page);
    await openScreen(page, { system_confirm_seconds: 12, system_swap_ms: 3_000 });
    await expect(page.getByTestId('running-version')).toHaveText('1.0.0+0');
    await expect(page.getByTestId('running-confirmation')).toHaveText(t('sysupd.confirmed'));

    await uploadAndVerify(page, mcubootImage(NEW));
    await expect(page.getByTestId('system-upload-state')).toHaveText(t('update.upload_state.ready'), { timeout: 30_000 });
    await expect(page.getByTestId('system-image-version')).toHaveText(versionText(NEW));
    await expect(page.getByTestId('system-image-compare')).toHaveAttribute('data-comparison', 'newer');

    await install(page);
    await expect(page.getByTestId('system-phase-requesting')).toHaveAttribute('data-status', /current|done/, { timeout: 10_000 });
    await expect(page.getByTestId('system-rebooting')).toBeVisible({ timeout: 15_000 });
    await signInAfterRestart(page);

    await expect(page.getByTestId('running-version')).toHaveText(versionText(NEW), { timeout: 10_000 });
    await expect(page.getByTestId('system-last-state')).toHaveText(t('sysupd.summary.awaiting_confirmation'));
    await expect(page.getByTestId('awaiting-warning')).toBeVisible();
    await expect(page.getByTestId('running-confirmation')).toContainText(t('sysupd.awaiting'));

    await expect(page.getByTestId('system-last-state')).toHaveText(t('sysupd.summary.succeeded'), { timeout: 30_000 });
    await expect(page.getByTestId('running-confirmation')).toHaveText(t('sysupd.confirmed'));
    await expect(page.getByTestId('awaiting-warning')).toHaveCount(0);
    expectRestartSilence(watch);
    watch.assertStaysHome(baseURL!);
  });

  test('a reset before confirmation returns the previous firmware and says so', async ({ page, baseURL }) => {
    test.setTimeout(120_000);
    const watch = watchPage(page);
    await openScreen(page, { system_swap_ms: 2_000 });
    await uploadAndVerify(page, mcubootImage(NEW));
    await expect(page.getByTestId('system-upload-state')).toHaveText(t('update.upload_state.ready'), { timeout: 30_000 });
    await install(page);
    await signInAfterRestart(page);
    await expect(page.getByTestId('system-last-state')).toHaveText(t('sysupd.summary.awaiting_confirmation'), { timeout: 10_000 });

    // While unconfirmed, slot 2 holds the way back: another upload is refused.
    await uploadAndVerify(page, mcubootImage({ ...NEW, revision: 1 }));
    await expect(page.getByTestId('system-upload-notice')).toHaveText(t('sysupd.upload_unconfirmed'));

    expect((await page.request.post('/__mock/reboot')).ok()).toBe(true);
    await signInAfterRestart(page);
    await expect(page.getByTestId('system-last-state')).toHaveText(t('sysupd.summary.rolled_back'), { timeout: 20_000 });
    await expect(page.getByTestId('running-version')).toHaveText('1.0.0+0');
    await expect(page.getByTestId('system-last-update')).toContainText(t('error.boot_changed'));
    expectRestartSilence(watch);
    watch.assertStaysHome(baseURL!);
  });

  test('an older image needs the downgrade acknowledged', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openScreen(page, { system_version: '2.0.0+0' });
    await uploadAndVerify(page, mcubootImage(NEW));
    await expect(page.getByTestId('system-image-compare')).toHaveAttribute('data-comparison', 'older', { timeout: 30_000 });
    const button = page.getByRole('button', { name: t('sysupd.install_submit') });
    await expect(button).toBeDisabled();
    await page.getByRole('checkbox', { name: t('sysupd.downgrade_acknowledge') }).check();
    await expect(button).toBeEnabled();
    watch.assertStaysHome(baseURL!);
  });

  test('a file that is not an MCUboot image is refused by verification, with the reason', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openScreen(page);
    const garbage = Buffer.alloc(20_000, 0x5a);
    await uploadAndVerify(page, garbage);
    await expect(page.getByTestId('system-verify-failed')).toContainText(t('error.invalid_image'), { timeout: 30_000 });
    await expect(page.getByRole('button', { name: t('sysupd.install_submit') })).toBeDisabled();
    watch.assertStaysHome(baseURL!);
  });

  test('an image with a damaged TLV hash is refused', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openScreen(page);
    const damaged = mcubootImage(NEW);
    damaged.writeUInt8(damaged.readUInt8(0x500) ^ 0xff, 0x500);
    await uploadAndVerify(page, damaged);
    await expect(page.getByTestId('system-verify-failed')).toContainText(t('error.invalid_image'), { timeout: 30_000 });
    watch.assertStaysHome(baseURL!);
  });

  test('while the running firmware is unconfirmed, the upload is refused and the page warns about resets', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await openScreen(page, { system_confirmed: false });
    await expect(page.getByTestId('awaiting-warning')).toBeVisible();
    await expect(page.getByTestId('system-install-unavailable')).toContainText(t('sysupd.reason.firmware_unconfirmed'));
    await uploadAndVerify(page, mcubootImage(NEW));
    await expect(page.getByTestId('system-upload-notice')).toHaveText(t('sysupd.upload_unconfirmed'));
    watch.assertStaysHome(baseURL!);
  });

  test('an upload cut short by a reload continues from the device offset', async ({ page, baseURL }) => {
    test.setTimeout(120_000);
    const watch = watchPage(page);
    await openScreen(page);
    const buffer = mcubootImage(NEW, 400_000);
    await page.getByLabel(t('sysupd.file_label')).setInputFiles(file(buffer));
    await expect(page.getByTestId('system-file-sha256')).toBeVisible({ timeout: 10_000 });
    await page.getByRole('button', { name: t('update.upload_submit') }).click();
    await expect
      .poll(async () => Number((await page.getByTestId('system-upload-progress').textContent())?.match(/\d+/)?.[0] ?? 0), {
        timeout: 20_000,
      })
      .toBeGreaterThan(32_000);

    await page.reload();
    await expect(page.getByTestId('system-upload-state')).toHaveText(t('update.upload_state.receiving'), { timeout: 10_000 });
    const resumedFrom = Number((await page.getByTestId('system-upload-progress').textContent())?.match(/\d+/)?.[0]);
    expect(resumedFrom).toBeGreaterThan(0);
    await page.getByLabel(t('sysupd.file_label')).setInputFiles(file(buffer));
    await page.getByRole('button', { name: t('update.upload_resume') }).click();
    await expect(page.getByTestId('system-upload-state')).toHaveText(t('update.upload_state.ready'), { timeout: 90_000 });
    await expect(page.getByTestId('system-image-version')).toHaveText(versionText(NEW));
    watch.assertStaysHome(baseURL!);
  });
});
