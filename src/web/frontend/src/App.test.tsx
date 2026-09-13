import { act, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { App } from './App';
import { t } from './i18n';
import { fakeDevice, json, refusal } from './test/server';
import {
  authStateConfigured,
  authStateFresh,
  coprocessorStatus,
  matterStatus,
  networkStatus,
  passwordAccepted,
  passwordJobRunning,
  session,
  systemStatus,
} from './test/fixtures';

beforeEach(() => {
  window.history.replaceState(null, '', '/');
});
afterEach(() => {
  window.history.replaceState(null, '', '/');
});

const signedInDevice = (extra: Record<string, Parameters<typeof fakeDevice>[0][string]> = {}) =>
  fakeDevice({
    'GET /api/v1/auth/state': () => json(200, authStateConfigured),
    'GET /api/v1/auth/session': () => json(200, session),
    'GET /api/v1/system/status': () => json(200, systemStatus),
    'GET /api/v1/network/status': () => json(200, networkStatus),
    'GET /api/v1/matter/status': () => json(200, matterStatus),
    'GET /api/v1/coprocessor/status': () => json(200, coprocessorStatus),
    'DELETE /api/v1/auth/session': () => new Response(null, { status: 204 }),
    ...extra,
  });

describe('setup', () => {
  it('shows the token and the HTTP warning, and sends both token and password', async () => {
    const device = fakeDevice({
      'GET /api/v1/auth/state': () => json(200, authStateFresh),
      'POST /api/v1/auth/setup': () => json(201, session),
      'GET /api/v1/system/status': () => json(200, systemStatus),
      'GET /api/v1/network/status': () => refusal(404, 'not_found'),
      'GET /api/v1/matter/status': () => refusal(404, 'not_found'),
      'GET /api/v1/coprocessor/status': () => refusal(404, 'not_found'),
    });
    render(<App />);
    expect(await screen.findByTestId('setup-token')).toHaveTextContent(authStateFresh.setup_token!);
    expect(screen.getByRole('note')).toHaveTextContent(t('app.http_warning'));

    await userEvent.type(screen.getByLabelText(t('setup.password_label')), 'a good long password');
    await userEvent.type(screen.getByLabelText(t('setup.password_confirm_label')), 'a good long password');
    await userEvent.click(screen.getByRole('button', { name: t('setup.submit') }));

    expect(await screen.findByRole('heading', { name: t('overview.title') })).toBeInTheDocument();
    const post = device.requests.find((r) => r.method === 'POST')!;
    expect(post.headers.get('X-Setup-Token')).toBe(authStateFresh.setup_token);
    expect(await post.json()).toEqual({ password: 'a good long password' });
  });

  it('checks length and confirmation before sending anything', async () => {
    const device = fakeDevice({ 'GET /api/v1/auth/state': () => json(200, authStateFresh) });
    render(<App />);
    await screen.findByTestId('setup-token');
    await userEvent.type(screen.getByLabelText(t('setup.password_label')), 'short');
    await userEvent.type(screen.getByLabelText(t('setup.password_confirm_label')), 'short');
    await userEvent.click(screen.getByRole('button', { name: t('setup.submit') }));
    expect(await screen.findByText(t('password.too_short', { min: 12 }))).toBeInTheDocument();

    await userEvent.clear(screen.getByLabelText(t('setup.password_label')));
    await userEvent.type(screen.getByLabelText(t('setup.password_label')), 'a good long password');
    await userEvent.click(screen.getByRole('button', { name: t('setup.submit') }));
    expect(await screen.findByText(t('password.mismatch'))).toBeInTheDocument();
    expect(device.requests.filter((r) => r.method === 'POST')).toHaveLength(0);
  });

  it("places the device's field error next to the input", async () => {
    fakeDevice({
      'GET /api/v1/auth/state': () => json(200, authStateFresh),
      'POST /api/v1/auth/setup': () => refusal(422, 'validation_failed', { fields: [{ path: '/password', code: 'too_long' }] }),
    });
    render(<App />);
    await screen.findByTestId('setup-token');
    await userEvent.type(screen.getByLabelText(t('setup.password_label')), 'a good long password');
    await userEvent.type(screen.getByLabelText(t('setup.password_confirm_label')), 'a good long password');
    await userEvent.click(screen.getByRole('button', { name: t('setup.submit') }));
    expect(await screen.findByText(t('field.too_long'))).toBeInTheDocument();
  });
});

describe('login', () => {
  it('says the password is wrong, and signs in with the right one', async () => {
    let attempts = 0;
    fakeDevice({
      'GET /api/v1/auth/state': () => json(200, authStateConfigured),
      'GET /api/v1/auth/session': () => refusal(401, 'authentication_required'),
      'POST /api/v1/auth/session': () => (++attempts === 1 ? refusal(401, 'invalid_credentials') : json(200, session)),
      'GET /api/v1/system/status': () => json(200, systemStatus),
      'GET /api/v1/network/status': () => json(200, networkStatus),
      'GET /api/v1/matter/status': () => json(200, matterStatus),
      'GET /api/v1/coprocessor/status': () => json(200, coprocessorStatus),
    });
    render(<App />);
    await userEvent.type(await screen.findByLabelText(t('login.password_label')), 'wrong one');
    await userEvent.click(screen.getByRole('button', { name: t('login.submit') }));
    expect(await screen.findByRole('alert')).toHaveTextContent(t('error.invalid_credentials'));

    await userEvent.type(screen.getByLabelText(t('login.password_label')), 'the right one');
    await userEvent.click(screen.getByRole('button', { name: t('login.submit') }));
    expect(await screen.findByRole('heading', { name: t('overview.title') })).toBeInTheDocument();
  });

  it('tells how long to wait when rate limited', async () => {
    fakeDevice({
      'GET /api/v1/auth/state': () => json(200, authStateConfigured),
      'GET /api/v1/auth/session': () => refusal(401, 'authentication_required'),
      'POST /api/v1/auth/session': () =>
        json(429, { error: { code: 'rate_limited', message: 'x', request_id: 'req_2', retryable: true } }, { 'Retry-After': '8' }),
    });
    render(<App />);
    await userEvent.type(await screen.findByLabelText(t('login.password_label')), 'guess');
    await userEvent.click(screen.getByRole('button', { name: t('login.submit') }));
    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent(t('error.rate_limited'));
    expect(alert).toHaveTextContent(t('error.retry_after', { seconds: 8 }));
    expect(alert).toHaveTextContent('req_2');
  });
});

describe('overview', () => {
  it('shows the device, and SSIDs as text', async () => {
    signedInDevice();
    render(<App />);
    expect(await screen.findByText(systemStatus.device_id)).toBeInTheDocument();
    expect(screen.getByText('1 д 2 ч')).toBeInTheDocument();
    expect(await screen.findByText('<b>k2</b>')).toBeInTheDocument();
    expect(document.querySelector('b')).toBeNull();
    expect(screen.getByText(t('overview.no_operations'))).toBeInTheDocument();
  });

  it('says what this firmware does not serve, and stops asking', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    try {
      const device = signedInDevice({
        'GET /api/v1/network/status': () => refusal(404, 'not_found'),
        'GET /api/v1/matter/status': () => refusal(404, 'not_found'),
        'GET /api/v1/coprocessor/status': () => refusal(404, 'not_found'),
      });
      render(<App />);
      await waitFor(() => expect(screen.getAllByText(t('overview.unavailable'))).toHaveLength(3));
      const count = (suffix: string) => device.requests.filter((r) => r.url.endsWith(suffix)).length;
      // Several polling intervals later: the status is still asked for, the
      // resources this firmware does not serve are not.
      await act(async () => {
        await vi.advanceTimersByTimeAsync(16_000);
      });
      expect(count('/system/status')).toBeGreaterThan(2);
      expect(count('/network/status')).toBe(1);
      expect(count('/matter/status')).toBe(1);
      expect(count('/coprocessor/status')).toBe(1);
    } finally {
      vi.useRealTimers();
    }
  });

  it('returns to login when the session ends while polling', async () => {
    let n = 0;
    signedInDevice({
      'GET /api/v1/system/status': () => (n++ === 0 ? json(200, systemStatus) : refusal(401, 'session_expired')),
    });
    render(<App />);
    await screen.findByText(systemStatus.device_id);
    expect(
      await screen.findByText(t('login.session_ended'), undefined, { timeout: 8000 }),
    ).toBeInTheDocument();
  }, 10000);
});

describe('access', () => {
  it('changes the password through its job and asks to sign in again', async () => {
    let polls = 0;
    const device = signedInDevice({
      'PUT /api/v1/auth/password': () => json(202, passwordAccepted),
      'GET /api/v1/jobs/job_00000001': () => (polls++ === 0 ? json(200, passwordJobRunning) : refusal(401, 'session_expired')),
    });
    window.history.replaceState(null, '', '/access');
    render(<App />);
    await userEvent.type(await screen.findByLabelText(t('access.current_password_label')), 'the old password');
    await userEvent.type(screen.getByLabelText(t('access.new_password_label')), 'a brand new password');
    await userEvent.type(screen.getByLabelText(t('access.new_password_confirm_label')), 'a brand new password');
    await userEvent.click(screen.getByRole('button', { name: t('access.change_submit') }));
    expect(await screen.findByText(t('login.password_changed'), undefined, { timeout: 5000 })).toBeInTheDocument();
    const put = device.requests.find((r) => r.method === 'PUT')!;
    expect(put.headers.get('X-CSRF-Token')).toBe(session.csrf_token);
    expect(await put.json()).toEqual({ current_password: 'the old password', new_password: 'a brand new password' });
  });

  it('keeps the idempotency key for a retry of the same change, and not for another', async () => {
    const keys: string[] = [];
    signedInDevice({
      'PUT /api/v1/auth/password': (request) => {
        keys.push(request.headers.get('Idempotency-Key')!);
        return refusal(500, 'internal_error');
      },
    });
    window.history.replaceState(null, '', '/access');
    render(<App />);
    const submit = async () => userEvent.click(screen.getByRole('button', { name: t('access.change_submit') }));
    await userEvent.type(await screen.findByLabelText(t('access.current_password_label')), 'the old password');
    await userEvent.type(screen.getByLabelText(t('access.new_password_label')), 'a brand new password');
    await userEvent.type(screen.getByLabelText(t('access.new_password_confirm_label')), 'a brand new password');
    await submit();
    await screen.findByRole('alert');
    await submit();
    await waitFor(() => expect(keys).toHaveLength(2));
    expect(keys[1]).toBe(keys[0]);

    await userEvent.type(screen.getByLabelText(t('access.new_password_label')), '!');
    await userEvent.type(screen.getByLabelText(t('access.new_password_confirm_label')), '!');
    await submit();
    await waitFor(() => expect(keys).toHaveLength(3));
    expect(keys[2]).not.toBe(keys[0]);
  });

  it('reports a wrong current password at that field', async () => {
    signedInDevice({ 'PUT /api/v1/auth/password': () => refusal(401, 'invalid_credentials') });
    window.history.replaceState(null, '', '/access');
    render(<App />);
    await userEvent.type(await screen.findByLabelText(t('access.current_password_label')), 'not the password');
    await userEvent.type(screen.getByLabelText(t('access.new_password_label')), 'a brand new password');
    await userEvent.type(screen.getByLabelText(t('access.new_password_confirm_label')), 'a brand new password');
    await userEvent.click(screen.getByRole('button', { name: t('access.change_submit') }));
    expect(await screen.findByText(t('error.invalid_credentials'))).toBeInTheDocument();
    expect(screen.getByLabelText(t('access.current_password_label'))).toHaveAttribute('aria-invalid', 'true');
  });
});
