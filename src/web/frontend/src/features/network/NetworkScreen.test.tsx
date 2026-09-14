import { render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { App } from '../../App';
import { type MessageKey, t } from '../../i18n';
import {
  authStateConfigured,
  capabilities,
  networkApplyAccepted,
  networkCommitSucceeded,
  networkConfig,
  networkDiscardAccepted,
  networkDiscardSucceeded,
  networkRollbackSucceeded,
  networkRuntime,
  networkRuntimeRadioAbsent,
  scanAccepted,
  scanResults,
  scanResultsTruncated,
  scanSucceeded,
  session,
  txAwaiting,
  txAwaitingDhcp,
  txCommitted,
  txRolledBack,
  txStaged,
  txTimedOut,
} from '../../test/fixtures';
import { schemaErrors } from '../../test/schema';
import { fakeDevice, type Handler, json, refusal } from '../../test/server';

beforeEach(() => {
  window.history.replaceState(null, '', '/network');
});
afterEach(() => {
  window.history.replaceState(null, '', '/');
});

const TX = '/api/v1/network/transactions/nettx_0001';

function networkDevice(extra: Record<string, Handler> = {}) {
  return fakeDevice({
    'GET /api/v1/auth/state': () => json(200, authStateConfigured),
    'GET /api/v1/auth/session': () => json(200, session),
    'GET /api/v1/capabilities': () => json(200, capabilities),
    'GET /api/v1/network/status': () => json(200, networkRuntime),
    'GET /api/v1/network/config': () => json(200, networkConfig),
    ...extra,
  });
}

/** A configuration with a change pending, as a reload finds it. */
const pendingConfig = () => json(200, { ...networkConfig, pending_transaction_id: 'nettx_0001' });

const scanRoutes = (results = scanResults): Record<string, Handler> => ({
  'POST /api/v1/network/wifi/scans': () => json(202, scanAccepted, { Location: scanAccepted.job_url }),
  'GET /api/v1/jobs/job_00000020': () => json(200, scanSucceeded),
  'GET /api/v1/network/wifi/scans/job_00000020': () => json(200, results),
});

const region = async (name: MessageKey) => within(await screen.findByRole('region', { name: t(name) }));
const requestsTo = (device: ReturnType<typeof fakeDevice>, method: string, suffix: string) =>
  device.requests.filter((r) => r.method === method && new URL(r.url).pathname.endsWith(suffix));
const stageButton = () => screen.getByRole('button', { name: t('network.stage_submit') });

async function typeStaticEthernet(gateway: string) {
  const ethernet = await region('network.ethernet_form');
  await userEvent.selectOptions(ethernet.getByLabelText(t('network.ipv4_mode')), 'static');
  await userEvent.type(ethernet.getByLabelText(t('network.address')), '192.168.88.50');
  await userEvent.type(ethernet.getByLabelText(t('network.prefix')), '24');
  await userEvent.type(ethernet.getByLabelText(t('network.gateway')), gateway);
  return ethernet;
}

async function scan() {
  const wifi = await region('network.wifi_form');
  await userEvent.click(wifi.getByRole('button', { name: t('network.scan_submit') }));
  const table = within(await screen.findByTestId('scan-results', {}, { timeout: 5_000 }));
  const row = (text: string) => within(table.getByText(text).closest('tr')!);
  return { wifi, table, row };
}

describe('network: state and form', () => {
  it('shows what the interfaces do now apart from the committed configuration, and the password as never shown', async () => {
    networkDevice();
    render(<App />);
    expect(await screen.findByRole('heading', { level: 1, name: t('network.title') })).toBeInTheDocument();
    expect(screen.getAllByRole('link').map((a) => a.textContent)).toEqual([
      t('nav.overview'),
      t('nav.matter'),
      t('nav.network'),
      t('nav.logs'),
      t('nav.access'),
    ]);

    const now = await region('network.runtime');
    expect(await now.findByText('fe80::8234:28ff:fe10:1273/64')).toBeInTheDocument();
    expect(now.getByText(t('address.source.link_local'))).toBeInTheDocument();
    expect(now.getByText(t('address.source.slaac'))).toBeInTheDocument();
    expect(now.getByText('<b>k2</b>')).toBeInTheDocument();
    expect(document.querySelector('b')).toBeNull();
    expect(now.getByText(t('value.dbm', { value: -57 }))).toBeInTheDocument();
    expect(now.getByText('2001:db8::1')).toBeInTheDocument();

    expect(await screen.findByTestId('config-revision')).toHaveTextContent('3');
    const wifi = await region('network.wifi_form');
    expect(wifi.getByLabelText(t('network.ssid'))).toHaveValue('Cedar-Lab');
    expect(wifi.getByLabelText(t('network.password'))).toHaveValue('');
    expect(wifi.getByText(t('network.password_keep_hint'))).toBeInTheDocument();
  });

  it('stages the edited candidate with the stored password kept, and compares the addresses', async () => {
    const device = networkDevice({
      'POST /api/v1/network/transactions': () => json(201, txStaged, { Location: TX }),
      [`GET ${TX}`]: () => json(200, txStaged),
    });
    render(<App />);
    await typeStaticEthernet('192.168.88.1');
    await userEvent.click(stageButton());

    expect(await screen.findByTestId('future-ethernet')).toHaveTextContent('192.168.88.50/24');
    expect(screen.getByTestId('current-ethernet')).toHaveTextContent('192.168.88.14/24');
    expect(screen.getByTestId('transaction-state')).toHaveTextContent(t('network.tx.staged'));
    expect(await screen.findByTestId('transaction-remaining')).toHaveTextContent(/^4 мин 4[5-7] с$/);
    expect(stageButton()).toBeDisabled();

    const [post] = requestsTo(device, 'POST', '/network/transactions');
    expect(post!.headers.get('X-CSRF-Token')).toBe(session.csrf_token);
    expect(post!.headers.get('Idempotency-Key')).toMatch(/^[A-Za-z0-9_-]{16,64}$/);
    const body = await post!.json();
    expect(schemaErrors('NetworkTransactionRequest', body)).toEqual([]);
    const { password_set: _, ...wifi } = txStaged.candidate.interfaces.wifi;
    expect(body).toEqual({
      base_revision: 3,
      config: { ...txStaged.candidate, interfaces: { ...txStaged.candidate.interfaces, wifi: { ...wifi, credential: { action: 'keep' } } } },
    });
  });

  it('asks for a password when the security changes instead of sending keep', async () => {
    const device = networkDevice({
      'POST /api/v1/network/transactions': () => json(201, txStaged),
      [`GET ${TX}`]: () => json(200, txStaged),
    });
    render(<App />);
    const wifi = await region('network.wifi_form');
    await userEvent.selectOptions(wifi.getByLabelText(t('network.security')), 'wpa3_sae');
    expect(wifi.getByText(t('network.password_new_hint'))).toBeInTheDocument();
    await userEvent.click(stageButton());
    expect(await wifi.findByText(t('network.password_required'))).toBeInTheDocument();
    expect(wifi.getByLabelText(t('network.password'))).toHaveAttribute('aria-invalid', 'true');
    expect(requestsTo(device, 'POST', '/network/transactions')).toHaveLength(0);

    await userEvent.type(wifi.getByLabelText(t('network.password')), 'a new password');
    await userEvent.click(stageButton());
    await waitFor(() => expect(requestsTo(device, 'POST', '/network/transactions')).toHaveLength(1));
    const body = await requestsTo(device, 'POST', '/network/transactions')[0]!.json();
    expect(body.config.interfaces.wifi).toMatchObject({
      security: 'wpa3_sae',
      credential: { action: 'replace', value: 'a new password' },
    });
  });

  it('keeps the key for a retry of the same candidate, and not for a changed one', async () => {
    const device = networkDevice({
      'POST /api/v1/network/transactions': () => {
        throw new TypeError('connection reset');
      },
    });
    render(<App />);
    const ethernet = await typeStaticEthernet('192.168.88.1');
    const keys = () => requestsTo(device, 'POST', '/network/transactions').map((r) => r.headers.get('Idempotency-Key'));

    await userEvent.click(stageButton());
    await screen.findByRole('alert');
    await userEvent.click(stageButton());
    await waitFor(() => expect(keys()).toHaveLength(2));
    expect(keys()[1]).toBe(keys()[0]);

    await userEvent.clear(ethernet.getByLabelText(t('network.gateway')));
    await userEvent.click(stageButton());
    await waitFor(() => expect(keys()).toHaveLength(3));
    expect(keys()[2]).not.toBe(keys()[0]);
  });

  it("places the device's field errors next to their inputs, and lists the ones no input owns", async () => {
    networkDevice({
      'POST /api/v1/network/transactions': () =>
        refusal(422, 'validation_failed', {
          fields: [
            { path: '/config/interfaces/ethernet/ipv4/gateway', code: 'out_of_range' },
            { path: '/config/interfaces/wifi/enabled', code: 'not_allowed' },
            { path: '/config/something_new', code: 'unknown_field' },
          ],
        }),
    });
    render(<App />);
    const ethernet = await typeStaticEthernet('10.0.0.1');
    await userEvent.click(stageButton());

    const gateway = ethernet.getByLabelText(t('network.gateway'));
    await waitFor(() => expect(gateway).toHaveAttribute('aria-invalid', 'true'));
    expect(gateway.getAttribute('aria-describedby')).toContain('ethernet-gateway-error');
    expect(document.getElementById('ethernet-gateway-error')).toHaveTextContent(t('field.out_of_range'));
    expect(ethernet.getByLabelText(t('network.address'))).not.toHaveAttribute('aria-invalid');
    expect(document.getElementById('wifi-enabled-error')).toHaveTextContent(t('field.not_allowed'));
    expect(screen.getByTestId('unplaced-fields')).toHaveTextContent('/config/something_new');
    expect(screen.queryByTestId('transaction-state')).toBeNull();
  });

  it('says the configuration changed when the revision is stale, and stages again from the re-read one', async () => {
    let revision = 3;
    const device = networkDevice({
      'GET /api/v1/network/config': () => json(200, { ...networkConfig, revision }),
      'POST /api/v1/network/transactions': async (request) =>
        (await request.json()).base_revision === revision ? json(201, txStaged) : refusal(409, 'stale_revision'),
      [`GET ${TX}`]: () => json(200, txStaged),
    });
    render(<App />);
    await region('network.ethernet_form');
    revision = 4;
    await userEvent.click(stageButton());
    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent(t('network.stale_revision'));
    await userEvent.click(within(alert).getByRole('button', { name: t('network.reread') }));
    await waitFor(() => expect(screen.queryByText(t('network.stale_revision'))).toBeNull());
    await userEvent.click(stageButton());

    expect(await screen.findByTestId('transaction-state')).toHaveTextContent(t('network.tx.staged'));
    const bases = await Promise.all(requestsTo(device, 'POST', '/network/transactions').map(async (r) => (await r.json()).base_revision));
    expect(bases).toEqual([3, 4]);
  });

  it('offers to open the transaction another browser has in progress', async () => {
    let pending: string | null = null;
    networkDevice({
      'GET /api/v1/network/config': () => json(200, { ...networkConfig, pending_transaction_id: pending }),
      'POST /api/v1/network/transactions': () => {
        pending = 'nettx_0007';
        return refusal(409, 'busy');
      },
      'GET /api/v1/network/transactions/nettx_0007': () => json(200, { ...txAwaiting, id: 'nettx_0007' }),
    });
    render(<App />);
    await region('network.ethernet_form');
    await userEvent.click(stageButton());
    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent(t('network.busy'));
    await userEvent.click(within(alert).getByRole('button', { name: t('network.open_transaction') }));

    expect(await screen.findByTestId('transaction-id')).toHaveTextContent('nettx_0007');
    expect(await screen.findByTestId('transaction-state')).toHaveTextContent(t('network.tx.awaiting_confirmation'));
    expect(stageButton()).toBeDisabled();
    expect(screen.getByText(t('network.form_locked'))).toBeInTheDocument();
  });

  it('hints at manual DNS once every enabled interface is static', async () => {
    networkDevice();
    render(<App />);
    await typeStaticEthernet('192.168.88.1');
    expect(screen.queryByTestId('dns-hint')).toBeNull();
    const wifi = await region('network.wifi_form');
    await userEvent.selectOptions(wifi.getByLabelText(t('network.ipv4_mode')), 'static');
    expect(screen.getByTestId('dns-hint')).toHaveTextContent(t('network.dns_static_hint'));
    const dns = await region('network.dns_form');
    await userEvent.selectOptions(dns.getByLabelText(t('network.dns_mode')), 'manual');
    expect(screen.queryByTestId('dns-hint')).toBeNull();
  });
});

describe('network: Wi-Fi scan', () => {
  it('lists every access point as text, one row per BSSID, and offers only the ones it can join', async () => {
    const device = networkDevice(scanRoutes());
    render(<App />);
    const { wifi, table, row } = await scan();

    expect(table.getAllByText('Cedar-Lab')).toHaveLength(2);
    expect(table.getByText('a4:2b:b0:99:88:77')).toBeInTheDocument();
    expect(table.getByText('Ce�d')).toBeInTheDocument();
    expect(table.getByText('<i>x</i>')).toBeInTheDocument();
    expect(document.querySelector('i')).toBeNull();
    expect(table.getByText(t('network.hidden_network'))).toBeInTheDocument();
    expect(row('corp-eap').getByRole('button', { name: t('network.ap_pick') })).toBeDisabled();
    expect(row('legacy-wep').getByRole('button', { name: t('network.ap_pick') })).toBeDisabled();
    expect(row('corp-eap').getByText(t('network.ap_unsupported'))).toBeInTheDocument();
    expect(row('Кедр').getByRole('button', { name: t('network.ap_pick') })).toBeEnabled();
    expect(screen.queryByTestId('scan-truncated')).toBeNull();

    const [post] = requestsTo(device, 'POST', '/network/wifi/scans');
    expect(post!.headers.get('X-CSRF-Token')).toBe(session.csrf_token);
    expect(post!.headers.get('Idempotency-Key')).toBeTruthy();
    expect(await post!.json()).toEqual({});

    await userEvent.click(row('Кедр').getByRole('button', { name: t('network.ap_pick') }));
    expect(wifi.getByLabelText(t('network.ssid'))).toHaveValue('Кедр');
    expect(wifi.getByLabelText(t('network.security'))).toHaveValue('wpa3_sae');
    expect(wifi.getByText(t('network.password_new_hint'))).toBeInTheDocument();

    await userEvent.click(row(t('network.hidden_network')).getByRole('button', { name: t('network.ap_pick') }));
    expect(wifi.getByLabelText(t('network.hidden'))).toBeChecked();
    expect(wifi.getByLabelText(t('network.ssid'))).toHaveValue('');
    expect(wifi.getByLabelText(t('network.ssid'))).toHaveFocus();
  });

  it('stages a picked network with its exact bytes and the typed password', async () => {
    const device = networkDevice({
      ...scanRoutes(),
      'POST /api/v1/network/transactions': () => json(201, txStaged),
      [`GET ${TX}`]: () => json(200, txStaged),
    });
    render(<App />);
    const { wifi, row } = await scan();
    await userEvent.click(row('Ce�d').getByRole('button', { name: t('network.ap_pick') }));
    expect(wifi.getByText(t('network.ssid_preserved'), { exact: false })).toBeInTheDocument();
    await userEvent.type(wifi.getByLabelText(t('network.password')), 'the wifi password');
    await userEvent.click(stageButton());

    await waitFor(() => expect(requestsTo(device, 'POST', '/network/transactions')).toHaveLength(1));
    const body = await requestsTo(device, 'POST', '/network/transactions')[0]!.json();
    expect(schemaErrors('NetworkTransactionRequest', body)).toEqual([]);
    expect(body.config.interfaces.wifi).toMatchObject({
      enabled: true,
      ssid_base64: 'Q2X/ZA==',
      security: 'wpa2_psk',
      hidden: false,
      credential: { action: 'replace', value: 'the wifi password' },
    });
  });

  it('says when the device cut the list short', async () => {
    networkDevice(scanRoutes(scanResultsTruncated));
    render(<App />);
    await scan();
    expect(screen.getByTestId('scan-truncated')).toHaveTextContent(t('network.scan_truncated', { count: 10 }));
  });

  it('says a scan is refused while a change is being applied', async () => {
    networkDevice({ 'POST /api/v1/network/wifi/scans': () => refusal(409, 'busy') });
    render(<App />);
    const wifi = await region('network.wifi_form');
    await userEvent.click(wifi.getByRole('button', { name: t('network.scan_submit') }));
    expect(await wifi.findByRole('alert')).toHaveTextContent(t('network.scan_busy'));
  });

  it('shows Wi-Fi as unavailable with its reason when the coprocessor is not ready, and offers neither enabling nor scanning', async () => {
    const disabled = structuredClone(networkConfig);
    disabled.config.interfaces.wifi.enabled = false;
    networkDevice({
      'GET /api/v1/network/status': () => json(200, networkRuntimeRadioAbsent),
      'GET /api/v1/network/config': () => json(200, disabled),
    });
    render(<App />);
    expect(await screen.findByTestId('wifi-unavailable')).toHaveTextContent(t('network.wifi_unavailable'));
    const wifi = await region('network.wifi_form');
    expect(wifi.getByLabelText(t('network.wifi_enabled'))).toBeDisabled();
    expect(wifi.getByRole('button', { name: t('network.scan_submit') })).toBeDisabled();
    expect(within(screen.getByTestId('runtime-wifi')).getByText(t('error.capability_unavailable'))).toBeInTheDocument();
  });

  it('turns Wi-Fi unavailable when a scan finds no radio', async () => {
    networkDevice({ 'POST /api/v1/network/wifi/scans': () => refusal(503, 'capability_unavailable') });
    render(<App />);
    const wifi = await region('network.wifi_form');
    await userEvent.click(wifi.getByRole('button', { name: t('network.scan_submit') }));
    expect(await screen.findByTestId('wifi-unavailable')).toBeInTheDocument();
    expect(wifi.getByRole('button', { name: t('network.scan_submit') })).toBeDisabled();
    // Switching an enabled Wi-Fi off stays possible.
    expect(wifi.getByLabelText(t('network.wifi_enabled'))).toBeEnabled();
  });
});

describe('network: apply and confirm', () => {
  it('recovers a staged change on load, applies it, counts down from the device, and confirms it', async () => {
    let state: 'staged' | 'awaiting' | 'committed' = 'staged';
    let awaitingReads = 0;
    const device = networkDevice({
      'GET /api/v1/network/config': () => (state === 'committed' ? json(200, { ...networkConfig, revision: 4 }) : pendingConfig()),
      [`GET ${TX}`]: () => {
        if (state === 'staged') return json(200, txStaged);
        if (state === 'committed') return json(200, txCommitted);
        // The device's clock: a timer kept by the page would not jump from 37 to 12.
        return json(200, { ...txAwaiting, remaining_seconds: awaitingReads++ < 2 ? 37 : 12 });
      },
      [`POST ${TX}/apply`]: () => {
        state = 'awaiting';
        return json(202, networkApplyAccepted);
      },
      [`POST ${TX}/confirm`]: () => {
        state = 'committed';
        return json(202, networkApplyAccepted);
      },
      'GET /api/v1/jobs/job_00000010': () => json(200, networkCommitSucceeded),
    });
    render(<App />);
    expect(await screen.findByTestId('transaction-state')).toHaveTextContent(t('network.tx.staged'));
    await userEvent.click(screen.getByRole('button', { name: t('network.apply_submit') }));

    await waitFor(() => expect(screen.getByTestId('transaction-state')).toHaveTextContent(t('network.tx.awaiting_confirmation')));
    expect(await screen.findByTestId('transaction-remaining')).toHaveTextContent(/^3[67] с$/);
    await waitFor(() => expect(screen.getByTestId('transaction-remaining')).toHaveTextContent(/^1[12] с$/), { timeout: 4_000 });
    expect(screen.getByTestId('reconnect-link')).toHaveAttribute('href', 'http://192.168.88.50/network?txn=nettx_0001');
    expect(await requestsTo(device, 'POST', '/apply')[0]!.json()).toEqual({ confirmation_timeout_seconds: 120 });

    await userEvent.click(screen.getByRole('button', { name: t('network.confirm_submit') }));
    expect(await screen.findByTestId('transaction-outcome', {}, { timeout: 5_000 })).toHaveTextContent(t('network.committed_notice'));
    const [confirm] = requestsTo(device, 'POST', '/confirm');
    expect(confirm!.headers.get('X-CSRF-Token')).toBe(session.csrf_token);
    expect(await confirm!.json()).toEqual({});
    // The form starts again from the new revision.
    await waitFor(() => expect(stageButton()).toBeEnabled());
  }, 15_000);

  it('keeps the change open when confirm is refused before the new settings work, and retries under the same key', async () => {
    let confirms = 0;
    let committed = false;
    const device = networkDevice({
      'GET /api/v1/network/config': pendingConfig,
      [`GET ${TX}`]: () => json(200, committed ? txCommitted : txAwaiting),
      [`POST ${TX}/confirm`]: () => {
        if (++confirms === 1) return refusal(409, 'invalid_state');
        committed = true;
        return json(202, networkApplyAccepted);
      },
      'GET /api/v1/jobs/job_00000010': () => json(200, networkCommitSucceeded),
    });
    render(<App />);
    const confirm = await screen.findByRole('button', { name: t('network.confirm_submit') });
    await userEvent.click(confirm);
    expect(await screen.findByRole('alert')).toHaveTextContent(t('network.confirm_not_ready'));
    expect(screen.getByTestId('transaction-state')).toHaveTextContent(t('network.tx.awaiting_confirmation'));
    await waitFor(() => expect(screen.getByRole('button', { name: t('network.confirm_submit') })).toBeEnabled());

    await userEvent.click(screen.getByRole('button', { name: t('network.confirm_submit') }));
    expect(await screen.findByTestId('transaction-outcome', {}, { timeout: 5_000 })).toHaveTextContent(t('network.committed_notice'));
    const keys = requestsTo(device, 'POST', '/confirm').map((r) => r.headers.get('Idempotency-Key'));
    expect(keys).toHaveLength(2);
    expect(keys[1]).toBe(keys[0]);
  }, 10_000);

  it('says the device rolled back by itself when nobody confirmed, and stops reading the transaction', async () => {
    let reads = 0;
    const device = networkDevice({
      'GET /api/v1/network/config': pendingConfig,
      [`GET ${TX}`]: () => json(200, reads++ < 2 ? txAwaiting : txTimedOut),
    });
    render(<App />);
    expect(await screen.findByTestId('transaction-outcome', {}, { timeout: 5_000 })).toHaveTextContent(t('network.timeout_notice'));
    expect(screen.queryByRole('button', { name: t('network.confirm_submit') })).toBeNull();
    const count = requestsTo(device, 'GET', '/nettx_0001').length;
    await new Promise((resolve) => setTimeout(resolve, 2_500));
    expect(requestsTo(device, 'GET', '/nettx_0001')).toHaveLength(count);
  }, 10_000);

  it('rolls back an applied change through its apply job', async () => {
    let rolledBack = false;
    const device = networkDevice({
      'GET /api/v1/network/config': pendingConfig,
      [`GET ${TX}`]: () => json(200, rolledBack ? txRolledBack : txAwaiting),
      [`DELETE ${TX}`]: () => {
        rolledBack = true;
        return json(202, networkApplyAccepted);
      },
      'GET /api/v1/jobs/job_00000010': () => json(200, networkRollbackSucceeded),
    });
    render(<App />);
    await userEvent.click(await screen.findByRole('button', { name: t('network.rollback_submit') }));
    expect(await screen.findByTestId('transaction-outcome', {}, { timeout: 5_000 })).toHaveTextContent(t('network.rolled_back_notice'));
    const [deleted] = requestsTo(device, 'DELETE', '/nettx_0001');
    expect(deleted!.headers.get('X-CSRF-Token')).toBe(session.csrf_token);
    expect(deleted!.headers.get('Idempotency-Key')).toBeTruthy();
  }, 10_000);

  it('discards a staged change through its own job', async () => {
    let discarded = false;
    networkDevice({
      'GET /api/v1/network/config': pendingConfig,
      [`GET ${TX}`]: () => json(200, discarded ? { ...txRolledBack, job_id: 'job_00000011' } : txStaged),
      [`DELETE ${TX}`]: () => {
        discarded = true;
        return json(202, networkDiscardAccepted);
      },
      'GET /api/v1/jobs/job_00000011': () => json(200, networkDiscardSucceeded),
    });
    render(<App />);
    await userEvent.click(await screen.findByRole('button', { name: t('network.discard_submit') }));
    expect(await screen.findByTestId('transaction-outcome', {}, { timeout: 5_000 })).toHaveTextContent(t('network.rolled_back_notice'));
    await userEvent.click(screen.getByRole('button', { name: t('network.close_transaction') }));
    expect(screen.queryByTestId('transaction-state')).toBeNull();
    expect(stageButton()).toBeEnabled();
  }, 10_000);
});

describe('network: after an address change', () => {
  it('opens the transaction named by the link, and explains a DHCP address it cannot link to', async () => {
    window.history.replaceState(null, '', '/network?txn=nettx_0001');
    const device = networkDevice({ [`GET ${TX}`]: () => json(200, txAwaitingDhcp) });
    render(<App />);
    expect(await screen.findByTestId('transaction-id')).toHaveTextContent('nettx_0001');
    expect(screen.getByTestId('reconnect-dhcp')).toHaveTextContent('/network?txn=nettx_0001');
    expect(screen.queryByTestId('reconnect-link')).toBeNull();
    expect(screen.getByTestId('future-ethernet')).toHaveTextContent(t('network.future_dhcp'));
    expect(requestsTo(device, 'GET', '/nettx_0001').length).toBeGreaterThan(0);
  });

  it('says when the named transaction is gone, and drops it from the address', async () => {
    window.history.replaceState(null, '', '/network?txn=nettx_0099');
    networkDevice({ 'GET /api/v1/network/transactions/nettx_0099': () => refusal(404, 'not_found') });
    render(<App />);
    expect(await screen.findByTestId('transaction-missing')).toHaveTextContent('nettx_0099');
    expect(stageButton()).toBeEnabled();
    await userEvent.click(screen.getByRole('button', { name: t('network.close_transaction') }));
    expect(window.location.search).toBe('');
    expect(window.location.pathname).toBe('/network');
  });
});
