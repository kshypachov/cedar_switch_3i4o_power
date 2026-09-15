import { createHash } from 'node:crypto';

import { render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { App } from '../../App';
import type { Job, Upload } from '../../api/types';
import { t } from '../../i18n';
import {
  accepted,
  authStateConfigured,
  capabilitiesUpdater,
  chunkSucceeded,
  coprocessorBridgeBusy,
  coprocessorEmpty,
  coprocessorInterrupted,
  coprocessorUpdated,
  deleteSucceeded,
  installFailed,
  installPreflight,
  installSucceeded,
  installWriting,
  session,
  uploadFailed,
  uploadImage,
  uploadReceiving,
  verifyFailed,
  verifySucceeded,
} from '../../test/fixtures';
import { schemaErrors } from '../../test/schema';
import { fakeDevice, type Handler, json, refusal } from '../../test/server';
import { STORAGE_KEY } from './remember';

beforeEach(() => {
  window.history.replaceState(null, '', '/coprocessor');
  window.localStorage.clear();
});
afterEach(() => {
  window.history.replaceState(null, '', '/');
  window.localStorage.clear();
});

const SIZE = 40_000;
const UPLOAD = '/api/v1/firmware/uploads/upload_0001';

function image(size = SIZE): Uint8Array<ArrayBuffer> {
  const bytes = new Uint8Array(size);
  for (let i = 0; i < size; i++) bytes[i] = (i * 31 + 7) & 0xff;
  bytes[0] = 0xe9;
  return bytes;
}

const sha = (bytes: Uint8Array) => createHash('sha256').update(bytes).digest('hex');

/** A device with an upload store: chunks land at their offsets, verify makes the file ready. */
function updateDevice(options: { upload?: Upload | null; status?: Handler; extra?: Record<string, Handler> } = {}) {
  let stored: Upload | null = options.upload ?? null;
  const chunks: { offset: number; bytes: Uint8Array; key: string | null; type: string | null }[] = [];
  const creates: unknown[] = [];
  const device = fakeDevice({
    'GET /api/v1/auth/state': () => json(200, authStateConfigured),
    'GET /api/v1/auth/session': () => json(200, session),
    'GET /api/v1/coprocessor/status': options.status ?? (() => json(200, coprocessorEmpty)),
    'GET /api/v1/capabilities': () => json(200, capabilitiesUpdater),
    'POST /api/v1/firmware/uploads': async (request) => {
      const body = (await request.json()) as { filename: string; size_bytes: number; sha256: string };
      creates.push(body);
      stored = { ...uploadReceiving, filename: body.filename, size_bytes: body.size_bytes, sha256: body.sha256 };
      return json(201, stored);
    },
    [`GET ${UPLOAD}`]: () => (stored ? json(200, stored) : refusal(404, 'not_found')),
    [`PUT ${UPLOAD}/data`]: async (request, url) => {
      const offset = Number(url.searchParams.get('offset'));
      const bytes = new Uint8Array(await request.arrayBuffer());
      chunks.push({
        offset,
        bytes,
        key: request.headers.get('Idempotency-Key'),
        type: request.headers.get('Content-Type'),
      });
      if (!stored || offset !== stored.received_bytes) return refusal(409, 'offset_mismatch');
      stored = { ...stored, received_bytes: offset + bytes.length };
      return json(202, accepted('job_00000101', UPLOAD));
    },
    'GET /api/v1/jobs/job_00000101': () => json(200, chunkSucceeded),
    [`POST ${UPLOAD}/verify`]: () => {
      stored = { ...stored!, state: 'ready', image: uploadImage };
      return json(202, accepted('job_00000102', UPLOAD));
    },
    'GET /api/v1/jobs/job_00000102': () => json(200, verifySucceeded),
    [`DELETE ${UPLOAD}`]: () => {
      stored = null;
      return json(202, accepted('job_00000103', null));
    },
    'GET /api/v1/jobs/job_00000103': () => json(200, deleteSucceeded),
    ...options.extra,
  });
  return { device, chunks, creates, stored: () => stored };
}

async function chooseFile(bytes: Uint8Array<ArrayBuffer>, name = 'merged-binary.bin') {
  const input = await screen.findByLabelText(t('update.file_label'));
  await userEvent.upload(input, new File([bytes], name, { type: 'application/octet-stream' }));
  await screen.findByTestId('file-sha256', {}, { timeout: 5_000 });
}

const phaseStatuses = () =>
  ['preflight', 'entering_bootloader', 'begin', 'writing', 'verifying', 'reconnecting', 'health_check', 'complete'].map(
    (p) => screen.getByTestId(`phase-${p}`).getAttribute('data-status'),
  );

describe('ESP32 status', () => {
  it('shows what the module reports, OTA as unavailable with its reason, and no update so far', async () => {
    updateDevice({ status: () => json(200, coprocessorUpdated) });
    render(<App />);
    expect(await screen.findByRole('heading', { level: 1, name: t('update.title') })).toBeInTheDocument();
    expect(await screen.findByTestId('coprocessor-version')).toHaveTextContent('v3.0.6');
    expect(screen.getByTestId('coprocessor-state')).toHaveTextContent(t('coprocessor.ready'));
    expect(screen.getByTestId('coprocessor-uart')).toHaveTextContent(t('logs.uart_mode.console'));
    expect(screen.getByTestId('method-ota')).toHaveTextContent(
      t('update.unavailable', { reason: t('update.reason.not_implemented') }),
    );
    expect(screen.getByTestId('method-uart')).toHaveTextContent(t('update.available'));
    expect(screen.getByTestId('last-update-state')).toHaveTextContent(t('update.summary.succeeded'));
    // No switch pretends OTA could be chosen.
    expect(screen.queryByRole('checkbox', { name: t('update.method_ota') })).toBeNull();
  });

  it('says an interrupted update may leave the module unbootable, with the device reason', async () => {
    updateDevice({ status: () => json(200, coprocessorInterrupted) });
    render(<App />);
    expect(await screen.findByTestId('last-update-state')).toHaveTextContent(t('update.summary.interrupted'));
    expect(screen.getByTestId('recovery-required')).toHaveTextContent(t('update.recovery_required'));
    expect(screen.getByTestId('last-update')).toHaveTextContent(t('error.boot_changed'));
  });

  it('says why a write is unavailable while the UART is lent to the USB bridge', async () => {
    updateDevice({ status: () => json(200, coprocessorBridgeBusy), upload: { ...uploadReceiving, received_bytes: SIZE, state: 'ready', image: uploadImage } });
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0001' }));
    render(<App />);
    expect(await screen.findByTestId('install-unavailable')).toHaveTextContent(t('update.reason.uart_usb_bridge'));
    await userEvent.click(await screen.findByRole('checkbox', { name: t('update.install_acknowledge') }));
    expect(screen.getByRole('button', { name: t('update.install_submit') })).toBeDisabled();
  });

  it('says what this firmware does not serve', async () => {
    updateDevice({
      status: () => refusal(404, 'not_found'),
      extra: { 'GET /api/v1/capabilities': () => refusal(404, 'not_found') },
    });
    render(<App />);
    expect(await screen.findByText(t('overview.unavailable'))).toBeInTheDocument();
  });
});

describe('the file', () => {
  it('refuses a file larger than the device publishes, before any request', async () => {
    const { device } = updateDevice();
    render(<App />);
    await screen.findByTestId('method-uart');
    await waitFor(() => expect(device.requests.some((r) => r.url.endsWith('/capabilities'))).toBe(true));
    const input = screen.getByLabelText(t('update.file_label'));
    const tooBig = new File([new Uint8Array(capabilitiesUpdater.limits.upload_max_bytes + 1)], 'huge.bin');
    await userEvent.upload(input, tooBig);
    expect(await screen.findByRole('alert')).toHaveTextContent(
      t('update.file_too_large', { max: capabilitiesUpdater.limits.upload_max_bytes }),
    );
    expect(screen.getByRole('button', { name: t('update.upload_submit') })).toBeDisabled();
    expect(device.requests.some((r) => r.url.includes('/firmware/'))).toBe(false);
  });

  it('shows the SHA-256 it computed here', async () => {
    updateDevice();
    render(<App />);
    const bytes = image();
    await chooseFile(bytes);
    expect(screen.getByTestId('file-sha256')).toHaveTextContent(sha(bytes));
  });

  it('refuses an empty file', async () => {
    const { device } = updateDevice();
    render(<App />);
    await screen.findByTestId('method-uart');
    await userEvent.upload(screen.getByLabelText(t('update.file_label')), new File([], 'empty.bin'));
    expect(await screen.findByRole('alert')).toHaveTextContent(t('update.file_empty'));
    expect(screen.getByRole('button', { name: t('update.upload_submit') })).toBeDisabled();
    expect(device.requests.some((r) => r.url.includes('/firmware/'))).toBe(false);
  });
});

describe('upload and verify', () => {
  it('creates the upload, sends raw chunks one job at a time, verifies and shows the image', async () => {
    const dev = updateDevice();
    render(<App />);
    const bytes = image();
    await chooseFile(bytes);
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
    expect(await screen.findByTestId('image-version', {}, { timeout: 10_000 })).toHaveTextContent('1');
    expect(screen.getByTestId('upload-state')).toHaveTextContent(t('update.upload_state.ready'));

    expect(dev.creates).toEqual([{ filename: 'merged-binary.bin', size_bytes: SIZE, sha256: sha(bytes) }]);
    expect(schemaErrors('UploadRequest', dev.creates[0])).toEqual([]);
    expect(dev.chunks.map((c) => [c.offset, c.bytes.length])).toEqual([
      [0, 16384],
      [16384, 16384],
      [32768, 7232],
    ]);
    for (const c of dev.chunks) {
      expect(c.type).toBe('application/octet-stream');
      expect(c.bytes).toEqual(bytes.subarray(c.offset, c.offset + c.bytes.length));
      expect(c.key).toMatch(/^[A-Za-z0-9_-]{16,64}$/);
    }
    expect(new Set(dev.chunks.map((c) => c.key)).size).toBe(3);
    expect(JSON.parse(window.localStorage.getItem(STORAGE_KEY)!)).toMatchObject({ uploadId: 'upload_0001' });
  });

  it('continues a remembered upload from what the device received', async () => {
    const dev = updateDevice({ upload: { ...uploadReceiving, received_bytes: 16384 } });
    const bytes = image();
    // The remembered upload carries the digest of this very file.
    dev.stored()!.sha256 = sha(bytes);
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0001' }));
    render(<App />);
    expect(await screen.findByTestId('upload-progress')).toHaveTextContent('16384');
    await chooseFile(bytes);
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_resume') }));
    await screen.findByTestId('image-version', {}, { timeout: 10_000 });
    expect(dev.creates).toEqual([]);
    expect(dev.chunks.map((c) => c.offset)).toEqual([16384, 32768]);
  });

  it('forgets a remembered upload the device no longer has', async () => {
    updateDevice();
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0001' }));
    render(<App />);
    await waitFor(() => expect(window.localStorage.getItem(STORAGE_KEY)).toBeNull());
    expect(screen.queryByTestId('upload-state')).toBeNull();
  });

  it('does not overwrite another file left on the device, and deletes it on request', async () => {
    const dev = updateDevice({ upload: { ...uploadReceiving, received_bytes: 100, sha256: 'b'.repeat(64) } });
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0001' }));
    render(<App />);
    await screen.findByTestId('upload-state');
    await chooseFile(image());
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
    expect(await screen.findByText(t('update.upload_mismatch'))).toBeInTheDocument();
    expect(dev.chunks).toEqual([]);

    await userEvent.click(screen.getByRole('button', { name: t('update.delete_submit') }));
    await waitFor(() => expect(screen.queryByTestId('upload-state')).toBeNull());
    expect(dev.stored()).toBeNull();
    expect(window.localStorage.getItem(STORAGE_KEY)).toBeNull();
  });

  it('says when another browser holds the upload slot', async () => {
    updateDevice({ extra: { 'POST /api/v1/firmware/uploads': () => refusal(409, 'busy') } });
    render(<App />);
    await chooseFile(image());
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
    expect(await screen.findByText(t('update.upload_busy_unknown'))).toBeInTheDocument();
  });

  it('shows why verification failed, in words and with the device message', async () => {
    const dev = updateDevice({
      extra: {
        [`POST ${UPLOAD}/verify`]: () => json(202, accepted('job_00000102', UPLOAD)),
        'GET /api/v1/jobs/job_00000102': () => json(200, verifyFailed),
      },
    });
    const original = dev.device.fetch.getMockImplementation()!;
    let verified = false;
    dev.device.fetch.mockImplementation(async (input: RequestInfo | URL, init?: RequestInit) => {
      const request = input instanceof Request ? input : new Request(input, init);
      if (request.method === 'POST' && request.url.endsWith('/verify')) verified = true;
      if (verified && request.method === 'GET' && request.url.endsWith('/upload_0001')) {
        return json(200, { ...uploadFailed, sha256: dev.stored()!.sha256 });
      }
      return original(request);
    });
    render(<App />);
    await chooseFile(image());
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
    const failed = await screen.findByTestId('verify-failed', {}, { timeout: 10_000 });
    expect(failed).toHaveTextContent(t('error.invalid_image'));
    expect(failed).toHaveTextContent('idf.py merge-bin');
    expect(screen.getByRole('button', { name: t('update.install_submit') })).toBeDisabled();
  });
});

describe('install', () => {
  const ready: Upload = { ...uploadReceiving, received_bytes: SIZE, state: 'ready', image: uploadImage };

  function installDevice(jobs: Job[], extra: Record<string, Handler> = {}) {
    let polls = 0;
    const bodies: unknown[] = [];
    const dev = updateDevice({
      upload: ready,
      extra: {
        'POST /api/v1/coprocessor/updates': async (request) => {
          bodies.push(await request.json());
          return json(202, accepted('job_00000105', '/api/v1/coprocessor/status'));
        },
        'GET /api/v1/jobs/job_00000105': () => json(200, jobs[Math.min(polls++, jobs.length - 1)]),
        ...extra,
      },
    });
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0001' }));
    return { ...dev, bodies };
  }

  it('asks first, then writes with the acknowledgement and follows every phase to the end', async () => {
    const dev = installDevice([installPreflight, installWriting, installSucceeded]);
    render(<App />);
    const install = await screen.findByRole('button', { name: t('update.install_submit') });
    await screen.findByTestId('image-version');
    expect(install).toBeDisabled();
    await userEvent.click(screen.getByRole('checkbox', { name: t('update.install_acknowledge') }));
    expect(install).toBeEnabled();
    await userEvent.click(install);

    await waitFor(() => expect(screen.getByTestId('phase-writing')).toHaveAttribute('data-status', 'current'), {
      timeout: 5_000,
    });
    expect(screen.getByTestId('phase-writing')).toHaveTextContent(
      t('update.progress_bytes', { completed: 262144, total: 1200000 }),
    );
    expect(await screen.findByTestId('install-notice', {}, { timeout: 5_000 })).toHaveTextContent(
      t('update.install_succeeded'),
    );
    expect(phaseStatuses()).toEqual(Array(8).fill('done'));

    expect(dev.bodies).toEqual([{ upload_id: 'upload_0001', method: 'uart', acknowledge_recovery: true }]);
    expect(schemaErrors('UpdateRequest', dev.bodies[0])).toEqual([]);
    expect(JSON.parse(window.localStorage.getItem(STORAGE_KEY)!).installJobId).toBeNull();
  });

  it('offers cancel only while the device says the job is cancellable, and says when it is too late', async () => {
    let cancels = 0;
    installDevice([installPreflight, installPreflight, installPreflight, installWriting, installWriting, installWriting, installSucceeded], {
      'POST /api/v1/jobs/job_00000105/cancel': () => {
        cancels++;
        return refusal(409, 'invalid_state');
      },
    });
    render(<App />);
    await userEvent.click(await screen.findByRole('checkbox', { name: t('update.install_acknowledge') }));
    await userEvent.click(screen.getByRole('button', { name: t('update.install_submit') }));
    await userEvent.click(await screen.findByRole('button', { name: t('update.cancel_submit') }));
    expect(await screen.findByText(t('update.cancel_refused'))).toBeInTheDocument();
    expect(cancels).toBe(1);
    // While writing - still running, no longer cancellable - there is no cancel.
    await waitFor(() => expect(screen.getByTestId('phase-writing')).toHaveAttribute('data-status', 'current'), {
      timeout: 5_000,
    });
    expect(screen.queryByRole('button', { name: t('update.cancel_submit') })).toBeNull();
  });

  it('shows a failed write with the device reason', async () => {
    installDevice([installPreflight, installFailed]);
    render(<App />);
    await userEvent.click(await screen.findByRole('checkbox', { name: t('update.install_acknowledge') }));
    await userEvent.click(screen.getByRole('button', { name: t('update.install_submit') }));
    const failed = await screen.findByTestId('install-failed', {}, { timeout: 5_000 });
    expect(failed).toHaveTextContent(t('error.internal_error'));
    expect(screen.getByTestId('phase-health_check')).toHaveAttribute('data-status', 'stopped');
  });

  it('says a write needs Ethernet in its own words', async () => {
    installDevice([], { 'POST /api/v1/coprocessor/updates': () => refusal(409, 'ethernet_required') });
    render(<App />);
    await userEvent.click(await screen.findByRole('checkbox', { name: t('update.install_acknowledge') }));
    await userEvent.click(screen.getByRole('button', { name: t('update.install_submit') }));
    expect(await screen.findByTestId('ethernet-required')).toHaveTextContent(t('update.ethernet_required'));
  });

  it('keeps following through a device that does not answer for a while', async () => {
    let polls = 0;
    const dev = installDevice([]);
    const original = dev.device.fetch.getMockImplementation()!;
    dev.device.fetch.mockImplementation(async (input: RequestInfo | URL, init?: RequestInit) => {
      const request = input instanceof Request ? input : new Request(input, init);
      if (request.url.endsWith('/jobs/job_00000105')) {
        polls++;
        if (polls === 2) throw new TypeError('Failed to fetch');
        return json(200, polls < 7 ? installWriting : installSucceeded);
      }
      return original(request);
    });
    render(<App />);
    await userEvent.click(await screen.findByRole('checkbox', { name: t('update.install_acknowledge') }));
    await userEvent.click(screen.getByRole('button', { name: t('update.install_submit') }));
    expect(await screen.findByText(t('update.connection_lost'), {}, { timeout: 5_000 })).toBeInTheDocument();
    // The next answer clears it while the write is still going.
    await waitFor(() => expect(screen.queryByText(t('update.connection_lost'))).toBeNull(), { timeout: 3_000 });
    expect(screen.queryByTestId('install-notice')).toBeNull();
    expect(await screen.findByTestId('install-notice', {}, { timeout: 8_000 })).toHaveTextContent(
      t('update.install_succeeded'),
    );
    expect(screen.queryByText(t('update.connection_lost'))).toBeNull();
  });

  it('after a reload follows the remembered job, and says when a reboot took it away', async () => {
    updateDevice({
      status: () => json(200, coprocessorInterrupted),
      extra: { 'GET /api/v1/jobs/job_00000105': () => refusal(404, 'not_found') },
    });
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify({ installJobId: 'job_00000105' }));
    render(<App />);
    expect(await screen.findByTestId('install-notice')).toHaveTextContent(t('update.job_gone'));
    expect(await screen.findByTestId('recovery-required')).toBeInTheDocument();
    expect(window.localStorage.getItem(STORAGE_KEY)).toBeNull();
  });

  it('signs out when the session is gone', async () => {
    updateDevice({ status: () => refusal(401, 'session_expired') });
    render(<App />);
    expect(await screen.findByRole('heading', { name: t('login.title') }, { timeout: 3_000 })).toBeInTheDocument();
  });

  it('nothing on the screen is written as markup', async () => {
    updateDevice({
      status: () =>
        json(200, {
          ...coprocessorInterrupted,
          last_update: { ...coprocessorInterrupted.last_update!, error: { ...coprocessorInterrupted.last_update!.error!, message: '<b>boom</b>' } },
        }),
    });
    render(<App />);
    const last = await screen.findByTestId('last-update');
    expect(within(last).getByText(/<b>boom<\/b>/)).toBeInTheDocument();
    expect(last.querySelector('b')).toBeNull();
  });
});
