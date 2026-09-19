import { render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { App } from '../../App';
import { t } from '../../i18n';
import {
  authStateConfigured,
  coprocessorBridge,
  coprocessorStatus,
  logPage,
  logPageAfterReboot,
  logPageNext,
  logSources,
  logSourcesBridge,
  session,
} from '../../test/fixtures';
import { fakeDevice, type Handler, json, refusal } from '../../test/server';

beforeEach(() => {
  window.history.replaceState(null, '', '/logs');
});
afterEach(() => {
  window.history.replaceState(null, '', '/');
});

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

function logsDevice(records: Handler, extra: Record<string, Handler> = {}) {
  return fakeDevice({
    'GET /api/v1/auth/state': () => json(200, authStateConfigured),
    'GET /api/v1/auth/session': () => json(200, session),
    'GET /api/v1/logs/sources': () => json(200, logSources),
    'GET /api/v1/coprocessor/status': () => json(200, coprocessorStatus),
    'GET /api/v1/logs/records': records,
    'GET /api/v1/system/coredump': () => json(200, { coredump: null }),
    ...extra,
  });
}

const recordRequests = (device: ReturnType<typeof fakeDevice>) =>
  device.requests.filter((r) => new URL(r.url).pathname === '/api/v1/logs/records').map((r) => new URL(r.url).searchParams);

/** Answers with the tail first, then with @p next for any request carrying a cursor. */
const tailThen = (next: () => Response) => (_request: Request, url: URL) =>
  url.searchParams.has('cursor') ? next() : json(200, logPage);

describe('logs', () => {
  it('shows the tail as text: levels, markers, a cut line, and the counters', async () => {
    const device = logsDevice(tailThen(() => json(200, { ...logPageNext, items: [] })));
    render(<App />);
    expect(await screen.findByRole('heading', { level: 1, name: t('logs.title') })).toBeInTheDocument();
    const log = await screen.findByRole('log');
    await within(log).findByText('invalid header: 0xffffffff');

    // Markup in a line is shown, not rendered.
    expect(within(log).getByText('<b>not markup</b>')).toBeInTheDocument();
    expect(log.querySelector('b')).toBeNull();
    expect(within(log).getByText(t('logs.truncated'))).toBeInTheDocument();
    expect(within(log).getByText(t('logs.kind.reset'))).toBeInTheDocument();
    expect(within(log).getByText(t('logs.kind.paused'))).toBeInTheDocument();
    expect(within(log).getByText(t('logs.level.unknown'))).toBeInTheDocument();
    expect(within(log).getByText('1:02:03.456')).toBeInTheDocument();
    expect(screen.getByTestId('log-boot-id')).toHaveTextContent(logPage.boot_id);

    expect(await screen.findByTestId('log-source-esp32')).toHaveTextContent('17');
    expect(screen.getByText(t('logs.uart_mode.console'))).toBeInTheDocument();

    const first = recordRequests(device)[0]!;
    expect(first.has('cursor')).toBe(false);
    expect(first.get('limit')).toBe('100');
  });

  it('polls on from the cursor the device returned', async () => {
    const device = logsDevice(tailThen(() => json(200, logPageNext)));
    render(<App />);
    expect(await screen.findByText('socket 0 reopened', {}, { timeout: 3_000 })).toBeInTheDocument();
    expect(recordRequests(device)[1]!.get('cursor')).toBe(logPage.next_cursor);
  });

  it('fetches again at once while the device says there is more', async () => {
    let calls = 0;
    const device = logsDevice(() => {
      calls++;
      return calls === 1 ? json(200, { ...logPage, has_more: true }) : json(200, logPageNext);
    });
    render(<App />);
    await waitFor(() => expect(recordRequests(device).length).toBeGreaterThanOrEqual(2), { timeout: 700 });
    expect(recordRequests(device)[1]!.get('cursor')).toBe(logPage.next_cursor);
    expect(await screen.findByText('socket 0 reopened')).toBeInTheDocument();
  });

  it('drops the cursor when a filter changes and asks for the tail of the new filter', async () => {
    const device = logsDevice(tailThen(() => json(200, { ...logPageNext, items: [] })));
    render(<App />);
    await screen.findByText('invalid header: 0xffffffff');
    await userEvent.selectOptions(screen.getByLabelText(t('logs.source_label')), 'esp32');
    await waitFor(() => {
      const last = recordRequests(device).at(-1)!;
      expect(last.get('source')).toBe('esp32');
      expect(last.has('cursor')).toBe(false);
    });
  });

  it('waits for typing to settle before searching', async () => {
    const device = logsDevice(tailThen(() => json(200, { ...logPageNext, items: [] })));
    render(<App />);
    await screen.findByText('invalid header: 0xffffffff');
    await userEvent.type(screen.getByLabelText(t('logs.contains_label')), 'header');
    await waitFor(() => expect(recordRequests(device).at(-1)!.get('contains')).toBe('header'), { timeout: 2_000 });
    expect(recordRequests(device).filter((q) => q.has('contains')).map((q) => q.get('contains'))).toEqual(['header']);
  });

  it('starts over from the tail when the device refuses the cursor', async () => {
    let refused = false;
    const device = logsDevice((_r, url) => {
      if (url.searchParams.has('cursor') && !refused) {
        refused = true;
        return refusal(400, 'invalid_cursor');
      }
      return json(200, url.searchParams.has('cursor') ? { ...logPageNext, items: [] } : logPage);
    });
    render(<App />);
    await screen.findByText('invalid header: 0xffffffff');
    await waitFor(() => expect(refused).toBe(true), { timeout: 3_000 });
    await waitFor(() => {
      const queries = recordRequests(device);
      const refusedAt = queries.findIndex((q) => q.has('cursor'));
      expect(queries[refusedAt + 1]?.has('cursor')).toBe(false);
    });
    expect(screen.queryByRole('alert')).toBeNull();
    expect(await screen.findByText('invalid header: 0xffffffff')).toBeInTheDocument();
  });

  it('shows a new boot and a gap where they happened', async () => {
    logsDevice(tailThen(() => json(200, logPageAfterReboot)));
    render(<App />);
    expect(await screen.findByTestId('log-boot', {}, { timeout: 3_000 })).toHaveTextContent('boot_fedcba9876543210');
    expect(screen.getByTestId('log-gap')).toHaveTextContent(t('logs.gap'));
    expect(screen.getByTestId('log-boot-id')).toHaveTextContent('boot_fedcba9876543210');
  });

  it('sends nothing while paused and resumes from the same cursor', async () => {
    const device = logsDevice(tailThen(() => json(200, { ...logPageNext, items: [] })));
    render(<App />);
    await screen.findByText('invalid header: 0xffffffff');
    await userEvent.click(screen.getByRole('button', { name: t('logs.pause') }));
    expect(screen.getByRole('status')).toHaveTextContent(t('logs.paused_notice'));
    const before = recordRequests(device).length;
    await sleep(1_500);
    expect(recordRequests(device)).toHaveLength(before);

    await userEvent.click(screen.getByRole('button', { name: t('logs.resume') }));
    await waitFor(() => expect(recordRequests(device).length).toBeGreaterThan(before));
    expect(recordRequests(device)[before]!.get('cursor')).toBe(logPage.next_cursor);
  });

  it('says why ESP32 logs stop while the UART is lent to the USB bridge', async () => {
    logsDevice(tailThen(() => json(200, { ...logPageNext, items: [] })), {
      'GET /api/v1/logs/sources': () => json(200, logSourcesBridge),
      'GET /api/v1/coprocessor/status': () => json(200, coprocessorBridge),
    });
    render(<App />);
    expect(await screen.findByTestId('log-source-esp32')).toHaveTextContent(
      t('logs.unavailable', { reason: t('logs.reason.uart_usb_bridge') }),
    );
    expect(await screen.findByText(t('logs.uart_mode.usb_bridge'))).toBeInTheDocument();
  });

  it('offers both exports with the current filters', async () => {
    logsDevice(tailThen(() => json(200, { ...logPageNext, items: [] })));
    render(<App />);
    await screen.findByText('invalid header: 0xffffffff');
    await userEvent.selectOptions(screen.getByLabelText(t('logs.level_label')), 'error');
    const ndjson = new URL(screen.getByTestId('log-export-ndjson').getAttribute('href')!, 'http://device');
    const text = new URL(screen.getByTestId('log-export-text').getAttribute('href')!, 'http://device');
    expect(ndjson.pathname).toBe('/api/v1/logs/export');
    expect(Object.fromEntries(ndjson.searchParams)).toEqual({ min_level: 'error', format: 'ndjson' });
    expect(text.searchParams.get('format')).toBe('text');
    expect(screen.getByTestId('log-export-text')).toHaveAttribute('download');
  });

  it('says what this firmware does not serve and stops asking', async () => {
    const device = logsDevice(() => refusal(404, 'not_found'), {
      'GET /api/v1/logs/sources': () => refusal(404, 'not_found'),
      'GET /api/v1/coprocessor/status': () => refusal(404, 'not_found'),
    });
    render(<App />);
    await waitFor(() => expect(screen.getAllByText(t('overview.unavailable')).length).toBeGreaterThanOrEqual(2));
    const count = recordRequests(device).length;
    await sleep(1_300);
    expect(recordRequests(device)).toHaveLength(count);
  });

  it('asks for the tail at once after a refused cursor, not a poll later', async () => {
    const calls: { cursor: boolean; at: number }[] = [];
    let refused = false;
    logsDevice((_r, url) => {
      const cursor = url.searchParams.has('cursor');
      calls.push({ cursor, at: performance.now() });
      if (cursor && !refused) {
        refused = true;
        return refusal(400, 'invalid_cursor');
      }
      return json(200, cursor ? { ...logPageNext, items: [] } : logPage);
    });
    render(<App />);
    await waitFor(() => expect(refused).toBe(true), { timeout: 3_000 });
    await waitFor(() => expect(calls.filter((c) => !c.cursor).length).toBeGreaterThanOrEqual(2), { timeout: 3_000 });
    const refusedAt = calls.findIndex((c) => c.cursor);
    expect(calls[refusedAt + 1]!.cursor).toBe(false);
    expect(calls[refusedAt + 1]!.at - calls[refusedAt]!.at).toBeLessThan(500);
  });

  it('does not apply a page that arrives after its filters changed', async () => {
    let release: (() => void) | undefined;
    logsDevice((_r, url) => {
      if (url.searchParams.get('source') === 'esp32' || url.searchParams.has('cursor')) {
        return json(200, { ...logPageNext, items: [] });
      }
      return new Promise<Response>((resolve) => {
        release = () => resolve(json(200, logPage));
      });
    });
    render(<App />);
    await waitFor(() => expect(release).toBeDefined(), { timeout: 3_000 });
    await userEvent.selectOptions(await screen.findByLabelText(t('logs.source_label')), 'esp32');
    await sleep(100);
    release!();
    await sleep(400);
    expect(screen.queryByText('invalid header: 0xffffffff')).toBeNull();
  });

  it('signs out when the session is gone', async () => {
    logsDevice(() => refusal(401, 'session_expired'));
    render(<App />);
    expect(await screen.findByRole('heading', { name: t('login.title') }, { timeout: 3_000 })).toBeInTheDocument();
  });
});

describe('coredump', () => {
  const stored = { coredump: { size_bytes: 30_000, reason: 'kernel_panic', reason_code: 4 } };

  it('says so when no dump is stored', async () => {
    logsDevice(() => json(200, logPage));
    render(<App />);
    const card = await screen.findByRole('region', { name: t('coredump.title') });
    expect(await within(card).findByText(t('coredump.none'))).toBeInTheDocument();
    expect(within(card).queryByTestId('coredump-download')).toBeNull();
  });

  it('describes a stored dump and links its download', async () => {
    logsDevice(() => json(200, logPage), { 'GET /api/v1/system/coredump': () => json(200, stored) });
    render(<App />);
    const card = await screen.findByRole('region', { name: t('coredump.title') });
    expect(await within(card).findByText(t('coredump.size_value', { kib: '29', bytes: 30000 }))).toBeInTheDocument();
    expect(within(card).getByText(`${t('coredump.reason.kernel_panic')} (4)`)).toBeInTheDocument();
    const link = within(card).getByTestId('coredump-download');
    expect(link).toHaveAttribute('href', '/api/v1/system/coredump/data');
    expect(link).toHaveAttribute('download');
  });

  it('shows the code when the header names no reason it knows', async () => {
    logsDevice(() => json(200, logPage), {
      'GET /api/v1/system/coredump': () => json(200, { coredump: { size_bytes: 512, reason: null, reason_code: null } }),
    });
    render(<App />);
    const card = await screen.findByRole('region', { name: t('coredump.title') });
    expect(await within(card).findByText(t('value.unknown'))).toBeInTheDocument();
    expect(within(card).getByText(t('coredump.size_value', { kib: '0.5', bytes: 512 }))).toBeInTheDocument();
  });

  it('clears the dump with the session CSRF token and reads the state again', async () => {
    let current: unknown = stored;
    const device = logsDevice(() => json(200, logPage), {
      'GET /api/v1/system/coredump': () => json(200, current),
      'DELETE /api/v1/system/coredump': () => {
        current = { coredump: null };
        return new Response(null, { status: 204 });
      },
    });
    render(<App />);
    const card = await screen.findByRole('region', { name: t('coredump.title') });
    await userEvent.click(await within(card).findByRole('button', { name: t('coredump.clear') }));
    expect(await within(card).findByText(t('coredump.none'))).toBeInTheDocument();
    const deleted = device.requests.find((r) => r.method === 'DELETE');
    expect(deleted?.headers.get('X-CSRF-Token')).toBe(session.csrf_token);
  });

  it('keeps the dump and says why when clearing fails', async () => {
    logsDevice(() => json(200, logPage), {
      'GET /api/v1/system/coredump': () => json(200, stored),
      'DELETE /api/v1/system/coredump': () => refusal(500, 'internal_error'),
    });
    render(<App />);
    const card = await screen.findByRole('region', { name: t('coredump.title') });
    await userEvent.click(await within(card).findByRole('button', { name: t('coredump.clear') }));
    expect(await within(card).findByRole('alert')).toBeInTheDocument();
    expect(within(card).getByTestId('coredump-download')).toBeInTheDocument();
  });

  it('says the firmware keeps no dump when the capability is missing', async () => {
    logsDevice(() => json(200, logPage), {
      'GET /api/v1/system/coredump': () => refusal(503, 'capability_unavailable'),
    });
    render(<App />);
    const card = await screen.findByRole('region', { name: t('coredump.title') });
    expect(await within(card).findByText(t('coredump.unavailable'))).toBeInTheDocument();
  });
});
