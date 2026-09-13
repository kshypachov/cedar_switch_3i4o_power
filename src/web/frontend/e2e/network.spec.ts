import { expect, type Page, test } from '@playwright/test';

import { MOCK_PASSWORD, resetDevice, t, watchPage } from './helpers';

// The mock's device: Ethernet on DHCP at 192.168.88.14, Wi-Fi disabled with no
// password, revision 1. Its jobs take real time (apply about 3.4 s, commit
// 1.5 s, rollback 2 s, scan 4 s); the confirmation timeout is reached with
// /__mock/advance, which the page itself never calls.

async function signIn(page: Page, path = '/network') {
  await page.goto(path);
  await page.getByLabel(t('login.password_label')).fill(MOCK_PASSWORD);
  await page.getByRole('button', { name: t('login.submit') }).click();
  await expect(page.getByRole('heading', { level: 1, name: t('network.title'), exact: true })).toBeVisible();
}

const region = (page: Page, key: Parameters<typeof t>[0]) => page.getByRole('region', { name: t(key), exact: true });

async function stageStaticEthernet(page: Page, address: string, gateway: string) {
  const ethernet = region(page, 'network.ethernet_form');
  await ethernet.getByLabel(t('network.ipv4_mode'), { exact: true }).selectOption('static');
  await ethernet.getByLabel(t('network.address'), { exact: true }).fill(address);
  await ethernet.getByLabel(t('network.prefix'), { exact: true }).fill('24');
  await ethernet.getByLabel(t('network.gateway'), { exact: true }).fill(gateway);
  await page.getByRole('button', { name: t('network.stage_submit') }).click();
}

async function applyStaged(page: Page) {
  await expect(page.getByTestId('transaction-state')).toHaveText(t('network.tx.staged'));
  await page.getByRole('button', { name: t('network.apply_submit') }).click();
  await expect(page.getByTestId('transaction-state')).toHaveText(t('network.tx.awaiting_confirmation'), { timeout: 10_000 });
}

test.describe('network', () => {
  test.beforeEach(async ({ page }) => {
    await resetDevice(page, { setup_required: false });
  });

  test('stages a static address next to the current one, applies and confirms it', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await signIn(page);
    await expect(page.getByTestId('config-revision')).toHaveText('1');
    await stageStaticEthernet(page, '192.168.88.50', '192.168.88.1');

    await expect(page.getByTestId('current-ethernet')).toHaveText('192.168.88.14/24');
    await expect(page.getByTestId('future-ethernet')).toHaveText('192.168.88.50/24');
    await page.getByLabel(t('network.confirm_timeout_label')).selectOption('120');
    await applyStaged(page);
    // The deadline is the device's and started at apply.
    await expect(page.getByTestId('transaction-remaining')).toHaveText(/^1 мин \d+ с$/);

    await page.getByRole('button', { name: t('network.confirm_submit') }).click();
    await expect(page.getByTestId('transaction-outcome')).toHaveText(t('network.committed_notice'), { timeout: 10_000 });
    await expect(page.getByTestId('config-revision')).toHaveText('2', { timeout: 10_000 });
    await expect(page.getByTestId('runtime-ethernet')).toContainText('192.168.88.50/24', { timeout: 10_000 });
    watch.assertStaysHome(baseURL!);
  });

  test("shows the device's refusal of a gateway next to the gateway field", async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await signIn(page);
    await stageStaticEthernet(page, '192.168.88.50', '10.0.0.1');

    const gateway = region(page, 'network.ethernet_form').getByLabel(t('network.gateway'), { exact: true });
    await expect(gateway).toHaveAttribute('aria-invalid', 'true');
    await expect(page.locator('#ethernet-gateway-error')).toHaveText(t('field.out_of_range'));
    await expect(page.getByTestId('transaction-state')).toHaveCount(0);
    watch.assertStaysHome(baseURL!);
  });

  test('scans, lists every access point, stages the one picked and discards it', async ({ page, baseURL }) => {
    const watch = watchPage(page);
    await signIn(page);
    const wifi = region(page, 'network.wifi_form');
    await wifi.getByRole('button', { name: t('network.scan_submit') }).click();

    const table = page.getByTestId('scan-results');
    await expect(table).toBeVisible({ timeout: 15_000 });
    await expect(table.getByRole('cell', { name: 'Cedar-Lab', exact: true })).toHaveCount(2);
    await expect(table.getByText(t('network.hidden_network'))).toBeVisible();
    await expect(table.getByRole('row').filter({ hasText: 'corp-eap' }).getByRole('button')).toBeDisabled();
    await expect(table.getByRole('row').filter({ hasText: 'legacy-wep' }).getByRole('button')).toBeDisabled();

    await table.getByRole('row').filter({ hasText: 'Кедр' }).getByRole('button', { name: t('network.ap_pick') }).click();
    await expect(wifi.getByLabel(t('network.ssid'), { exact: true })).toHaveValue('Кедр');
    await expect(wifi.getByLabel(t('network.security'), { exact: true })).toHaveValue('wpa3_sae');
    await expect(wifi.getByLabel(t('network.wifi_enabled'))).toBeChecked();
    await wifi.getByLabel(t('network.password'), { exact: true }).fill('a wifi password');
    await page.getByRole('button', { name: t('network.stage_submit') }).click();

    await expect(page.getByTestId('transaction-state')).toHaveText(t('network.tx.staged'));
    await expect(page.getByTestId('future-wifi')).toHaveText(t('network.future_dhcp'));
    await page.getByRole('button', { name: t('network.discard_submit') }).click();
    await expect(page.getByTestId('transaction-outcome')).toHaveText(t('network.rolled_back_notice'), { timeout: 10_000 });
    watch.assertStaysHome(baseURL!);
  });

  test('rolls back by itself when nobody confirms in time', async ({ page, request, baseURL }) => {
    const watch = watchPage(page);
    await signIn(page);
    await stageStaticEthernet(page, '192.168.88.50', '192.168.88.1');
    await applyStaged(page);

    const advanced = await request.post('/__mock/advance', { data: { seconds: 125 } });
    expect(advanced.ok()).toBe(true);
    await expect(page.getByTestId('transaction-outcome')).toHaveText(t('network.timeout_notice'), { timeout: 15_000 });
    await expect(page.getByRole('button', { name: t('network.confirm_submit') })).toHaveCount(0);
    await expect(page.getByTestId('config-revision')).toHaveText('1');
    watch.assertStaysHome(baseURL!);
  });

  test('carries the transaction to the new address, where a new sign-in opens it and rolls it back', async ({ page, browser, baseURL }) => {
    const watch = watchPage(page);
    await signIn(page);
    await stageStaticEthernet(page, '192.168.88.50', '192.168.88.1');
    await applyStaged(page);
    const id = await page.getByTestId('transaction-id').textContent();
    expect(id).toMatch(/^[A-Za-z0-9_-]+$/);
    await expect(page.getByTestId('reconnect-link')).toHaveAttribute('href', `http://192.168.88.50/network?txn=${id}`);

    // A reload without the parameter finds the change through pending_transaction_id.
    await page.reload();
    await expect(page.getByTestId('transaction-id')).toHaveText(id!);

    // The new address cannot be reached from here; a browser with no session,
    // opening the link's path on the mock, stands in for it.
    const elsewhere = await browser.newContext({ baseURL });
    const second = await elsewhere.newPage();
    const secondWatch = watchPage(second);
    await signIn(second, `/network?txn=${id}`);
    await expect(second.getByTestId('transaction-id')).toHaveText(id!);
    await expect(second.getByTestId('transaction-state')).toHaveText(t('network.tx.awaiting_confirmation'));
    await second.getByRole('button', { name: t('network.rollback_submit') }).click();
    await expect(second.getByTestId('transaction-outcome')).toHaveText(t('network.rolled_back_notice'), { timeout: 10_000 });
    secondWatch.assertStaysHome(baseURL!);
    await elsewhere.close();

    await expect(page.getByTestId('transaction-outcome')).toHaveText(t('network.rolled_back_notice'), { timeout: 10_000 });
    watch.assertStaysHome(baseURL!);
  });

  test('keeps the change open while the device refuses to confirm it, and confirms once it works', async ({ page, request, baseURL }) => {
    const watch = watchPage(page);
    await signIn(page);
    await stageStaticEthernet(page, '192.168.88.50', '192.168.88.1');
    await applyStaged(page);

    expect((await request.post('/__mock/scenario', { data: { network_health: 'unhealthy' } })).ok()).toBe(true);
    await page.getByRole('button', { name: t('network.confirm_submit') }).click();
    await expect(page.getByRole('alert')).toContainText(t('network.confirm_not_ready'));
    await expect(page.getByTestId('transaction-state')).toHaveText(t('network.tx.awaiting_confirmation'));

    expect((await request.post('/__mock/scenario', { data: { network_health: 'healthy' } })).ok()).toBe(true);
    await page.getByRole('button', { name: t('network.confirm_submit') }).click();
    await expect(page.getByTestId('transaction-outcome')).toHaveText(t('network.committed_notice'), { timeout: 10_000 });
    watch.assertStaysHome(baseURL!);
  });

  test('shows Wi-Fi as unavailable while the coprocessor is not ready', async ({ page, baseURL }) => {
    await resetDevice(page, { setup_required: false, coprocessor_state: 'offline' });
    const watch = watchPage(page);
    await signIn(page);
    await expect(page.getByTestId('wifi-unavailable')).toHaveText(t('network.wifi_unavailable'));
    const wifi = region(page, 'network.wifi_form');
    await expect(wifi.getByLabel(t('network.wifi_enabled'))).toBeDisabled();
    await expect(wifi.getByRole('button', { name: t('network.scan_submit') })).toBeDisabled();
    await expect(page.getByTestId('runtime-wifi')).toContainText(t('error.capability_unavailable'));
    watch.assertStaysHome(baseURL!);
  });
});
