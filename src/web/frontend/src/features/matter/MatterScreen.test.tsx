import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { App } from '../../App';
import { t } from '../../i18n';
import { fakeDevice, type Handler, json, refusal } from '../../test/server';
import {
  authStateConfigured,
  capabilities,
  fabrics,
  matterOpenAccepted,
  matterOpenSucceeded,
  matterStatus,
  onboardingCodes,
  session,
  windowClosed,
  windowOpenByController,
  windowOpenFromWeb,
} from '../../test/fixtures';

beforeEach(() => {
  window.history.replaceState(null, '', '/matter');
});
afterEach(() => {
  window.history.replaceState(null, '', '/');
});

function matterDevice(extra: Record<string, Handler> = {}) {
  return fakeDevice({
    'GET /api/v1/auth/state': () => json(200, authStateConfigured),
    'GET /api/v1/auth/session': () => json(200, session),
    'GET /api/v1/capabilities': () => json(200, capabilities),
    'GET /api/v1/matter/status': () => json(200, matterStatus),
    'GET /api/v1/matter/commissioning': () => json(200, windowClosed),
    'GET /api/v1/matter/fabrics': () => json(200, fabrics),
    ...extra,
  });
}

const codesRequests = (device: ReturnType<typeof fakeDevice>) =>
  device.requests.filter((r) => r.url.endsWith('/matter/onboarding-codes'));

describe('matter', () => {
  it('shows the state, a closed window without asking for codes, and fabrics as text', async () => {
    const device = matterDevice();
    render(<App />);
    expect(await screen.findByRole('heading', { name: t('matter.title') })).toBeInTheDocument();
    expect(await screen.findByText(t('matter.window_closed'))).toBeInTheDocument();
    expect(screen.getByText(t('matter.codes.window_closed'))).toBeInTheDocument();

    // A label is text: markup in it is shown, not rendered.
    expect(await screen.findByText('<b>lab</b>')).toBeInTheDocument();
    expect(screen.getByText(t('matter.no_label'))).toBeInTheDocument();
    expect(screen.getByText('0xFFF1')).toBeInTheDocument();
    expect(screen.getByText('00000000000000A7')).toBeInTheDocument();
    expect(codesRequests(device)).toHaveLength(0);
  });

  it('says so when the device is in no fabric', async () => {
    matterDevice({ 'GET /api/v1/matter/fabrics': () => json(200, { items: [], count: 0 }) });
    render(<App />);
    expect(await screen.findByText(t('matter.fabrics_empty'))).toBeInTheDocument();
  });

  it('opens a window through its job, then shows the QR, manual code and PIN apart, zeros kept', async () => {
    let open = false;
    const device = matterDevice({
      'GET /api/v1/matter/commissioning': () => json(200, open ? windowOpenFromWeb : windowClosed),
      'POST /api/v1/matter/commissioning': () => {
        open = true;
        return json(202, matterOpenAccepted, { Location: matterOpenAccepted.job_url });
      },
      'GET /api/v1/jobs/job_00000002': () => json(200, matterOpenSucceeded),
      'GET /api/v1/matter/onboarding-codes': () => json(200, onboardingCodes),
    });
    render(<App />);
    await screen.findByText(t('matter.window_closed'));
    await userEvent.selectOptions(screen.getByLabelText(t('matter.timeout_label')), '600');
    await userEvent.click(screen.getByRole('button', { name: t('matter.open_submit') }));

    expect(await screen.findByTestId('manual-code', {}, { timeout: 5_000 })).toHaveTextContent('01234567890');
    expect(screen.getByTestId('setup-passcode')).toHaveTextContent('00012345');
    expect(screen.getByRole('img', { name: t('matter.qr_label') })).toHaveAttribute('data-payload', onboardingCodes.qr_payload);
    expect(screen.getByText(t('matter.source.web'))).toBeInTheDocument();
    expect(screen.getByTestId('window-remaining')).toBeInTheDocument();

    const post = device.requests.find((r) => r.method === 'POST')!;
    expect(post.headers.get('X-CSRF-Token')).toBe(session.csrf_token);
    expect(post.headers.get('Idempotency-Key')).toBeTruthy();
    expect(await post.json()).toEqual({ mode: 'basic', timeout_seconds: 600 });
    await waitFor(() => expect(codesRequests(device)).toHaveLength(1));
  });

  it('says a window is already open when the device refuses with invalid_state', async () => {
    matterDevice({ 'POST /api/v1/matter/commissioning': () => refusal(409, 'invalid_state') });
    render(<App />);
    await screen.findByText(t('matter.window_closed'));
    await userEvent.click(screen.getByRole('button', { name: t('matter.open_submit') }));
    expect(await screen.findByRole('alert')).toHaveTextContent(t('matter.window_already_open'));
  });

  it('shows a controller window without codes and without a timer, and never asks for codes', async () => {
    const device = matterDevice({
      'GET /api/v1/matter/commissioning': () => json(200, windowOpenByController),
    });
    render(<App />);
    expect(await screen.findByText(t('matter.codes.passcode_unavailable'))).toBeInTheDocument();
    expect(screen.getByText(t('matter.mode.enhanced'))).toBeInTheDocument();
    expect(screen.getByText(t('matter.source.controller'))).toBeInTheDocument();
    expect(screen.getByText(t('matter.window_remaining_unknown'))).toBeInTheDocument();
    expect(screen.getByRole('button', { name: t('matter.close_submit') })).toBeEnabled();
    expect(codesRequests(device)).toHaveLength(0);
  });

  it('keeps the key for a retry of the same open, and not for another timeout', async () => {
    const device = matterDevice({
      'POST /api/v1/matter/commissioning': () => {
        throw new TypeError('connection reset');
      },
    });
    render(<App />);
    await screen.findByText(t('matter.window_closed'));
    const submit = () => userEvent.click(screen.getByRole('button', { name: t('matter.open_submit') }));
    const keys = () => device.requests.filter((r) => r.method === 'POST').map((r) => r.headers.get('Idempotency-Key'));

    await submit();
    await screen.findByRole('alert');
    await submit();
    await waitFor(() => expect(keys()).toHaveLength(2));
    expect(keys()[1]).toBe(keys()[0]);

    await userEvent.selectOptions(screen.getByLabelText(t('matter.timeout_label')), '900');
    await submit();
    await waitFor(() => expect(keys()).toHaveLength(3));
    expect(keys()[2]).not.toBe(keys()[0]);
  });

  it('does not offer to open a window while Matter is starting', async () => {
    matterDevice({
      'GET /api/v1/matter/status': () => json(200, { ...matterStatus, state: 'starting', commissioned: false, fabric_count: 0 }),
    });
    render(<App />);
    expect(await screen.findByText(t('matter.not_ready_notice'))).toBeInTheDocument();
    expect(await screen.findByRole('button', { name: t('matter.open_submit') })).toBeDisabled();
  });

  it('says what this firmware does not serve', async () => {
    matterDevice({
      'GET /api/v1/matter/status': () => refusal(404, 'not_found'),
      'GET /api/v1/matter/commissioning': () => refusal(404, 'not_found'),
      'GET /api/v1/matter/fabrics': () => refusal(404, 'not_found'),
    });
    render(<App />);
    await waitFor(() => expect(screen.getAllByText(t('overview.unavailable')).length).toBeGreaterThanOrEqual(3));
  });
});
