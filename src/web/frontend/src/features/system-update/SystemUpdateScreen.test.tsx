import { createHash } from 'node:crypto';

import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { App } from '../../App';
import type { Capabilities, Job, SystemFirmware, Upload } from '../../api/types';
import { formatDuration } from '../../components/format';
import { t } from '../../i18n';
import {
  accepted,
  authStateConfigured,
  capabilitiesSystemUpdate,
  chunkSucceeded,
  deleteSucceeded,
  session,
  systemFirmwareAwaiting,
  systemFirmwareConfirmed,
  systemFirmwareRolledBack,
  systemImage,
  systemInstallRebooting,
  systemInstallRequesting,
  systemStatus,
  systemUploadReady,
  systemUploadReceiving,
  uploadReady,
  verifySucceeded,
} from '../../test/fixtures';
import { schemaErrors } from '../../test/schema';
import { fakeDevice, type Handler, json, refusal } from '../../test/server';
import { SYSTEM_STORAGE_KEY } from './SystemUpdateScreen';

// Hashing, three chunks, verification and the install's job polls run on real
// timers: a whole path takes several seconds.
vi.setConfig({ testTimeout: 20_000 });

beforeEach(() => {
  window.history.replaceState(null, '', '/firmware');
  window.localStorage.clear();
});
afterEach(() => {
  window.history.replaceState(null, '', '/');
  window.localStorage.clear();
});

const SIZE = 40_000;
const UPLOAD = '/api/v1/firmware/uploads/upload_0002';
const BOOT_BEFORE = systemStatus.boot_id;
const BOOT_AFTER = 'boot_fedcba9876543210';

function image(size = SIZE): Uint8Array<ArrayBuffer> {
  const bytes = new Uint8Array(size);
  for (let i = 0; i < size; i++) bytes[i] = (i * 17 + 3) & 0xff;
  return bytes;
}

const sha = (bytes: Uint8Array) => createHash('sha256').update(bytes).digest('hex');

interface Options {
  firmware?: SystemFirmware;
  upload?: Upload | null;
  capabilities?: Capabilities;
  /** The version verification finds in the image. */
  version?: string;
  /** The ESP32's upload the device holds as well. */
  other?: Upload | null;
  /** An upload another browser creates just before this page's create: that one is refused busy. */
  appearOnCreate?: Upload;
  /** What the device answers on /system/status once it restarted. */
  afterReboot?: Handler;
  extra?: Record<string, Handler>;
}

/**
 * A device with an STM32 update: an upload store, the install job that parks in
 * `rebooting`, and a restart that brings another boot and the new firmware.
 */
function systemDevice(options: Options = {}) {
  let stored: Upload | null = options.upload ?? null;
  let other: Upload | null = options.other ?? null;
  let firmware = options.firmware ?? systemFirmwareConfirmed;
  let jobPolls = 0;
  const creates: unknown[] = [];
  const chunks: { offset: number; length: number }[] = [];
  const installs: unknown[] = [];
  const device = fakeDevice({
    'GET /api/v1/auth/state': () => json(200, authStateConfigured),
    'GET /api/v1/auth/session': () => json(200, session),
    'GET /api/v1/capabilities': () => json(200, options.capabilities ?? capabilitiesSystemUpdate),
    'GET /api/v1/system/firmware': () => json(200, firmware),
    'GET /api/v1/system/status': (request, url) => {
      if (jobPolls < 2) return json(200, systemStatus);
      if (options.afterReboot) return options.afterReboot(request, url);
      firmware = systemFirmwareAwaiting;
      return json(200, { ...systemStatus, boot_id: BOOT_AFTER, uptime_ms: '41000' });
    },
    'POST /api/v1/firmware/uploads': async (request) => {
      if (options.appearOnCreate && stored === null) {
        stored = options.appearOnCreate;
        return refusal(409, 'busy');
      }
      const body = (await request.json()) as { filename: string; size_bytes: number; sha256: string };
      creates.push(body);
      stored = { ...systemUploadReceiving, filename: body.filename, size_bytes: body.size_bytes, sha256: body.sha256 };
      return json(201, stored);
    },
    'GET /api/v1/firmware/uploads': () => json(200, { uploads: [stored, other].filter((u): u is Upload => u !== null) }),
    [`GET ${UPLOAD}`]: () => (stored ? json(200, stored) : refusal(404, 'not_found')),
    'DELETE /api/v1/firmware/uploads/upload_0001': () => {
      other = null;
      return json(202, accepted('job_00000104', null));
    },
    'GET /api/v1/jobs/job_00000104': () => json(200, deleteSucceeded),
    [`PUT ${UPLOAD}/data`]: async (request, url) => {
      const offset = Number(url.searchParams.get('offset'));
      const length = (await request.arrayBuffer()).byteLength;
      chunks.push({ offset, length });
      if (!stored || offset !== stored.received_bytes) return refusal(409, 'offset_mismatch');
      stored = { ...stored, received_bytes: offset + length };
      return json(202, accepted('job_00000101', UPLOAD));
    },
    'GET /api/v1/jobs/job_00000101': () => json(200, chunkSucceeded),
    [`POST ${UPLOAD}/verify`]: () => {
      stored = { ...stored!, state: 'ready', image: { ...systemImage, version: options.version ?? systemImage.version } };
      return json(202, accepted('job_00000102', UPLOAD));
    },
    'GET /api/v1/jobs/job_00000102': () => json(200, { ...verifySucceeded, resource_url: UPLOAD }),
    [`DELETE ${UPLOAD}`]: () => {
      stored = null;
      return json(202, accepted('job_00000103', null));
    },
    'GET /api/v1/jobs/job_00000103': () => json(200, deleteSucceeded),
    'POST /api/v1/system/updates': async (request) => {
      installs.push(await request.json());
      return json(202, accepted('job_00000201', '/api/v1/system/firmware'));
    },
    'GET /api/v1/jobs/job_00000201': () => json(200, jobPolls++ === 0 ? systemInstallRequesting : systemInstallRebooting),
    ...options.extra,
  });
  return { device, creates, chunks, installs, stored: () => stored };
}

async function chooseFile(bytes: Uint8Array<ArrayBuffer>, name = 'zephyr.signed.bin') {
  const input = await screen.findByLabelText(t('sysupd.file_label'));
  await userEvent.upload(input, new File([bytes], name, { type: 'application/octet-stream' }));
  await screen.findByTestId('system-file-sha256', {}, { timeout: 5_000 });
}

async function uploadAndVerify(bytes = image()) {
  await chooseFile(bytes);
  await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
  await screen.findByTestId('system-image-version', {}, { timeout: 10_000 });
}

const installButton = () => screen.getByRole('button', { name: t('sysupd.install_submit') });

describe('the running firmware', () => {
  it('shows the version, that it is confirmed, and no update so far', async () => {
    systemDevice();
    render(<App />);
    expect(await screen.findByRole('heading', { level: 1, name: t('sysupd.title') })).toBeInTheDocument();
    expect(await screen.findByTestId('running-version')).toHaveTextContent('1.0.0+0');
    expect(screen.getByTestId('running-confirmation')).toHaveTextContent(t('sysupd.confirmed'));
    expect(screen.queryByTestId('awaiting-warning')).toBeNull();
    expect(screen.getByText(t('sysupd.last_none'))).toBeInTheDocument();
    expect(screen.getByRole('link', { name: t('nav.firmware') })).toHaveAttribute('aria-current', 'page');
  });

  it('unconfirmed: warns that a reset returns the previous firmware, counts down, and blocks another install', async () => {
    systemDevice({ firmware: systemFirmwareAwaiting, upload: systemUploadReady });
    window.localStorage.setItem(SYSTEM_STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0002' }));
    render(<App />);
    expect(await screen.findByTestId('awaiting-warning')).toHaveTextContent(t('sysupd.awaiting_warning'));
    expect(screen.getByTestId('awaiting-warning')).toHaveTextContent('sysupd confirm');
    expect(screen.getByTestId('running-confirmation')).toHaveTextContent(t('sysupd.awaiting'));
    expect(screen.getByTestId('running-confirmation')).toHaveTextContent(
      t('sysupd.confirm_in', { time: formatDuration(systemFirmwareAwaiting.confirm_remaining_seconds!) }),
    );
    expect(screen.getByTestId('system-last-state')).toHaveTextContent(t('sysupd.summary.awaiting_confirmation'));
    expect(await screen.findByTestId('system-install-blocker')).toHaveTextContent(t('sysupd.install_needs_confirmed'));
    expect(installButton()).toBeDisabled();
  });

  it('explains a roll-back with the device reason', async () => {
    systemDevice({ firmware: systemFirmwareRolledBack });
    render(<App />);
    expect(await screen.findByTestId('system-last-state')).toHaveTextContent(t('sysupd.summary.rolled_back'));
    expect(screen.getByTestId('system-last-update')).toHaveTextContent(t('error.boot_changed'));
    expect(screen.getByTestId('system-last-update')).toHaveTextContent('1.1.0+0');
  });

  it('says why the update is unavailable, in words', async () => {
    systemDevice({
      firmware: { ...systemFirmwareConfirmed, update: { available: false, reason: 'update_running' } },
      upload: systemUploadReady,
    });
    window.localStorage.setItem(SYSTEM_STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0002' }));
    render(<App />);
    expect(await screen.findByTestId('system-install-unavailable')).toHaveTextContent(t('sysupd.reason.update_running'));
    await screen.findByTestId('system-image-version');
    expect(installButton()).toBeDisabled();
  });

  it('a capability the firmware lacks outranks the resource', async () => {
    systemDevice({
      capabilities: { ...capabilitiesSystemUpdate, features: { ...capabilitiesSystemUpdate.features, stm32_update: { available: false, reason: 'not_implemented' } } },
    });
    render(<App />);
    expect(await screen.findByTestId('system-install-unavailable')).toHaveTextContent(t('update.reason.not_implemented'));
  });
});

describe('the file', () => {
  it('refuses a file larger than slot 2, before any request', async () => {
    const { device } = systemDevice();
    render(<App />);
    await screen.findByTestId('running-version');
    await waitFor(() => expect(device.requests.some((r) => r.url.endsWith('/capabilities'))).toBe(true));
    const max = capabilitiesSystemUpdate.limits.system_upload_max_bytes;
    await userEvent.upload(screen.getByLabelText(t('sysupd.file_label')), new File([new Uint8Array(max + 1)], 'huge.bin'));
    expect(await screen.findByTestId('system-file-problem')).toHaveTextContent(t('sysupd.file_too_large', { max }));
    expect(screen.getByRole('button', { name: t('update.upload_submit') })).toBeDisabled();
    // Only the look at what the device holds, nothing that creates or writes.
    expect(device.requests.some((r) => r.url.includes('/firmware/') && r.method !== 'GET')).toBe(false);
  });

  it('a file just within slot 2 is taken', async () => {
    const small = { ...capabilitiesSystemUpdate, limits: { ...capabilitiesSystemUpdate.limits, system_upload_max_bytes: SIZE } };
    systemDevice({ capabilities: small });
    render(<App />);
    await screen.findByTestId('running-version');
    await waitFor(() => expect(screen.getByLabelText(t('sysupd.file_label'))).toBeEnabled());
    await new Promise((r) => setTimeout(r, 50));
    await chooseFile(image(SIZE));
    expect(screen.queryByTestId('system-file-problem')).toBeNull();
  });
});

describe('upload, verify and install', () => {
  it('uploads for the STM32, verifies, asks, installs and follows the device through its restart', async () => {
    const dev = systemDevice();
    render(<App />);
    const bytes = image();
    await uploadAndVerify(bytes);

    expect(dev.creates).toEqual([{ filename: 'zephyr.signed.bin', size_bytes: SIZE, sha256: sha(bytes), target: 'stm32u585' }]);
    expect(schemaErrors('UploadRequest', dev.creates[0])).toEqual([]);
    expect(dev.chunks.map((c) => [c.offset, c.length])).toEqual([
      [0, 16384],
      [16384, 16384],
      [32768, 7232],
    ]);
    expect(screen.getByTestId('system-image-version')).toHaveTextContent('1.1.0+0');
    expect(screen.getByTestId('system-image-compare')).toHaveAttribute('data-comparison', 'newer');
    expect(JSON.parse(window.localStorage.getItem(SYSTEM_STORAGE_KEY)!)).toMatchObject({ uploadId: 'upload_0002' });
    // The ESP32 screen's entry is not touched.
    expect(window.localStorage.getItem('cedar.coprocessor-update')).toBeNull();

    await waitFor(() => expect(installButton()).toBeEnabled());
    await userEvent.click(installButton());
    const confirm = await screen.findByTestId('system-confirm');
    expect(confirm).toHaveTextContent(t('sysupd.confirm_title', { version: '1.1.0+0' }));
    expect(confirm).toHaveTextContent(t('sysupd.confirm_text'));
    expect(dev.installs).toEqual([]);

    await userEvent.click(screen.getByRole('button', { name: t('sysupd.confirm_submit') }));
    expect(await screen.findByTestId('system-install-notice', {}, { timeout: 10_000 })).toHaveTextContent(t('sysupd.rebooted'));
    expect(dev.installs).toEqual([{ upload_id: 'upload_0002', acknowledge_downgrade: false }]);
    expect(schemaErrors('SystemUpdateRequest', dev.installs[0])).toEqual([]);
    expect(screen.getByTestId('system-phase-rebooting')).toHaveAttribute('data-status', 'current');
    expect(screen.getByTestId('system-phase-requesting')).toHaveAttribute('data-status', 'done');
    // Nothing to resume: the install is over and slot 2 belongs to the swap.
    expect(window.localStorage.getItem(SYSTEM_STORAGE_KEY)).toBeNull();
    // The card shows the old version until its next poll after the restart.
    await waitFor(() => expect(screen.getByTestId('running-version')).toHaveTextContent('1.1.0+0'), { timeout: 5_000 });
    await waitFor(() => expect(screen.getByTestId('awaiting-warning')).toBeInTheDocument(), { timeout: 5_000 });
  });

  it('the confirmation can be declined, and nothing is sent', async () => {
    const dev = systemDevice();
    render(<App />);
    await uploadAndVerify();
    await waitFor(() => expect(installButton()).toBeEnabled());
    await userEvent.click(installButton());
    await userEvent.click(await screen.findByRole('button', { name: t('sysupd.confirm_cancel') }));
    expect(screen.queryByTestId('system-confirm')).toBeNull();
    expect(installButton()).toBeEnabled();
    expect(dev.installs).toEqual([]);
  });

  it('an older image needs the downgrade acknowledged, and says so to the device', async () => {
    const dev = systemDevice({ version: '0.9.0+0' });
    render(<App />);
    await uploadAndVerify();
    expect(screen.getByTestId('system-image-compare')).toHaveTextContent(t('sysupd.compare.older', { running: '1.0.0+0' }));
    await waitFor(() => expect(screen.getByRole('checkbox', { name: t('sysupd.downgrade_acknowledge') })).toBeInTheDocument());
    expect(installButton()).toBeDisabled();
    await userEvent.click(screen.getByRole('checkbox', { name: t('sysupd.downgrade_acknowledge') }));
    expect(installButton()).toBeEnabled();
    await userEvent.click(installButton());
    await userEvent.click(await screen.findByRole('button', { name: t('sysupd.confirm_submit') }));
    await screen.findByTestId('system-install-notice', {}, { timeout: 10_000 });
    expect(dev.installs).toEqual([{ upload_id: 'upload_0002', acknowledge_downgrade: true }]);
  });

  it('a downgrade box left checked does not travel to an image that is no longer older', async () => {
    // Running 1.0.0 at first; after the box is checked the device reports 0.8.0, so 0.9.0 is newer.
    let running = systemFirmwareConfirmed;
    const dev = systemDevice({ version: '0.9.0+0', extra: { 'GET /api/v1/system/firmware': () => json(200, running) } });
    render(<App />);
    await uploadAndVerify();
    const box = await screen.findByRole('checkbox', { name: t('sysupd.downgrade_acknowledge') });
    await userEvent.click(box);
    running = { ...systemFirmwareConfirmed, running: { ...systemFirmwareConfirmed.running, version: '0.8.0+0' } };
    await waitFor(() => expect(screen.getByTestId('system-image-compare')).toHaveAttribute('data-comparison', 'newer'), { timeout: 5_000 });
    await userEvent.click(installButton());
    await userEvent.click(await screen.findByRole('button', { name: t('sysupd.confirm_submit') }));
    await waitFor(() => expect(dev.installs).toHaveLength(1), { timeout: 5_000 });
    expect(dev.installs).toEqual([{ upload_id: 'upload_0002', acknowledge_downgrade: false }]);
  });

  it('the confirmation panel goes away when the device stops allowing the install, and nothing is sent', async () => {
    let firmware = systemFirmwareConfirmed;
    const dev = systemDevice({ extra: { 'GET /api/v1/system/firmware': () => json(200, firmware) } });
    render(<App />);
    await uploadAndVerify();
    await waitFor(() => expect(installButton()).toBeEnabled());
    await userEvent.click(installButton());
    await screen.findByTestId('system-confirm');
    // Another browser installed meanwhile: a swap is pending.
    firmware = { ...systemFirmwareConfirmed, swap_pending: true };
    await waitFor(() => expect(screen.queryByTestId('system-confirm')).toBeNull(), { timeout: 5_000 });
    expect(screen.getByTestId('system-install-blocker')).toHaveTextContent(t('sysupd.install_swap_pending'));
    expect(installButton()).toBeDisabled();
    expect(dev.installs).toEqual([]);
  });

  it('the confirmation panel closes as soon as the request leaves, so it cannot be sent twice', async () => {
    let release: () => void = () => {};
    const gate = new Promise<void>((resolve) => {
      release = resolve;
    });
    const dev = systemDevice({
      extra: {
        'POST /api/v1/system/updates': async (request) => {
          dev.installs.push(await request.json());
          await gate;
          return json(202, accepted('job_00000201', '/api/v1/system/firmware'));
        },
      },
    });
    render(<App />);
    await uploadAndVerify();
    await waitFor(() => expect(installButton()).toBeEnabled());
    await userEvent.click(installButton());
    await userEvent.click(await screen.findByRole('button', { name: t('sysupd.confirm_submit') }));
    await waitFor(() => expect(dev.installs).toHaveLength(1));
    // The answer has not come yet: the panel is already gone and nothing offers a second request.
    expect(screen.queryByTestId('system-confirm')).toBeNull();
    expect(screen.queryByRole('button', { name: t('sysupd.confirm_submit') })).toBeNull();
    expect(installButton()).toBeDisabled();
    release();
    await screen.findByTestId('system-install-notice', {}, { timeout: 10_000 });
    expect(dev.installs).toHaveLength(1);
  });

  it('the countdown follows each report of the device, not the first one', async () => {
    const start = Date.now();
    const report = () => 1140 - Math.floor((Date.now() - start) / 1000);
    systemDevice({
      extra: { 'GET /api/v1/system/firmware': () => json(200, { ...systemFirmwareAwaiting, confirm_remaining_seconds: report() }) },
    });
    render(<App />);
    await screen.findByTestId('running-confirmation');
    // Past two polls and a few ticks: the text is the latest report counted down, give or take a second.
    await new Promise((r) => setTimeout(r, 4_500));
    const now = report();
    const shown = screen.getByTestId('running-confirmation').textContent ?? '';
    const accepted = [now, now + 1, now - 1].map((s) => t('sysupd.confirm_in', { time: formatDuration(s) }));
    expect(accepted.some((text) => shown.includes(text)), `${shown} is not ${now} s`).toBe(true);
  });

  it('a swap already pending blocks the install and says so', async () => {
    systemDevice({ firmware: { ...systemFirmwareConfirmed, swap_pending: true }, upload: systemUploadReady });
    window.localStorage.setItem(SYSTEM_STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0002' }));
    render(<App />);
    expect(await screen.findByTestId('swap-pending')).toHaveTextContent(t('sysupd.swap_pending'));
    await screen.findByTestId('system-image-version');
    expect(screen.getByTestId('system-install-blocker')).toHaveTextContent(t('sysupd.install_swap_pending'));
    expect(installButton()).toBeDisabled();
  });

  it('the same version is a reinstall, without an acknowledgement', async () => {
    systemDevice({ version: '1.0.0+0' });
    render(<App />);
    await uploadAndVerify();
    expect(screen.getByTestId('system-image-compare')).toHaveTextContent(t('sysupd.compare.same'));
    expect(screen.queryByRole('checkbox', { name: t('sysupd.downgrade_acknowledge') })).toBeNull();
    await waitFor(() => expect(installButton()).toBeEnabled());
  });

  it('an image the device rejects shows its reason and cannot be installed', async () => {
    const failed: Upload = {
      ...systemUploadReceiving,
      received_bytes: SIZE,
      state: 'failed',
      error: { code: 'invalid_image', message: 'The TLV SHA-256 does not match the image', request_id: 'req_1', retryable: false },
    };
    const job: Job = { ...verifySucceeded, state: 'failed', error: failed.error };
    let verified = false;
    const dev = systemDevice({
      extra: {
        [`POST ${UPLOAD}/verify`]: () => {
          verified = true;
          return json(202, accepted('job_00000102', UPLOAD));
        },
        'GET /api/v1/jobs/job_00000102': () => json(200, job),
        [`GET ${UPLOAD}`]: () => json(200, verified ? failed : { ...dev.stored()! }),
      },
    });
    render(<App />);
    await chooseFile(image());
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
    const notice = await screen.findByTestId('system-verify-failed', {}, { timeout: 10_000 });
    expect(notice).toHaveTextContent(t('error.invalid_image'));
    expect(notice).toHaveTextContent('TLV SHA-256');
    expect(installButton()).toBeDisabled();
  });

  it('while the running firmware is unconfirmed the device refuses the upload, and the page says why', async () => {
    const dev = systemDevice({ extra: { 'POST /api/v1/firmware/uploads': () => refusal(409, 'invalid_state') } });
    render(<App />);
    await chooseFile(image());
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
    expect(await screen.findByTestId('system-upload-notice')).toHaveTextContent(t('sysupd.upload_unconfirmed'));
    expect(dev.chunks).toEqual([]);
  });

  it('an ESP32 upload holds the slot: the page names it and deletes it', async () => {
    const dev = systemDevice({ other: uploadReady });
    render(<App />);
    const notice = await screen.findByTestId('other-upload');
    expect(notice).toHaveTextContent(t('update.target.esp32c6'));
    expect(notice).toHaveTextContent(uploadReady.filename);
    await userEvent.click(screen.getByRole('button', { name: t('update.other_upload_delete') }));
    await waitFor(() => expect(screen.queryByTestId('other-upload')).toBeNull());
    await uploadAndVerify();
    expect(dev.creates).toHaveLength(1);
  });

  it('busy on create shows the STM32 upload the device holds, started elsewhere', async () => {
    systemDevice({
      appearOnCreate: { ...systemUploadReceiving, received_bytes: 8192, sha256: 'c'.repeat(64), filename: 'other.bin' },
    });
    render(<App />);
    await chooseFile(image());
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
    expect(await screen.findByTestId('system-upload-notice')).toHaveTextContent(t('update.upload_found'));
    expect(screen.getByTestId('system-upload-state')).toHaveTextContent(t('update.upload_state.receiving'));
    expect(screen.getByRole('button', { name: t('update.delete_submit') })).toBeEnabled();
  });

  it('busy on create with the same file continues the upload the device holds', async () => {
    const bytes = image();
    const dev = systemDevice({ appearOnCreate: { ...systemUploadReceiving, received_bytes: 16384, sha256: sha(bytes) } });
    render(<App />);
    await chooseFile(bytes);
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
    await screen.findByTestId('system-image-version', {}, { timeout: 10_000 });
    expect(dev.chunks.map((c) => c.offset)).toEqual([16384, 32768]);
  });

  it('the device refusing a too large file after all is shown at the file', async () => {
    systemDevice({ extra: { 'POST /api/v1/firmware/uploads': () => refusal(413, 'payload_too_large') } });
    render(<App />);
    await chooseFile(image());
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_submit') }));
    expect(await screen.findByTestId('system-file-problem')).toHaveTextContent(
      t('sysupd.file_too_large', { max: capabilitiesSystemUpdate.limits.system_upload_max_bytes }),
    );
  });

  it.each([
    ['busy', 409, 'sysupd.busy'],
    ['validation_failed', 422, 'sysupd.downgrade_required'],
    ['unsupported_target', 422, 'sysupd.unsupported_target'],
    ['invalid_state', 409, 'sysupd.not_ready'],
  ] as const)('an install refused with %s is explained', async (code, status, key) => {
    systemDevice({ extra: { 'POST /api/v1/system/updates': () => refusal(status, code) } });
    render(<App />);
    await uploadAndVerify();
    await waitFor(() => expect(installButton()).toBeEnabled());
    await userEvent.click(installButton());
    await userEvent.click(await screen.findByRole('button', { name: t('sysupd.confirm_submit') }));
    expect(await screen.findByTestId('system-install-refused')).toHaveTextContent(t(key));
    expect(screen.queryByTestId('system-confirm')).toBeNull();
    expect(installButton()).toBeEnabled();
  });

  it('an unexpected refusal falls back to the generic notice', async () => {
    systemDevice({ extra: { 'POST /api/v1/system/updates': () => refusal(503, 'service_not_ready') } });
    render(<App />);
    await uploadAndVerify();
    await waitFor(() => expect(installButton()).toBeEnabled());
    await userEvent.click(installButton());
    await userEvent.click(await screen.findByRole('button', { name: t('sysupd.confirm_submit') }));
    expect(await screen.findByText(t('error.service_not_ready'))).toBeInTheDocument();
    expect(screen.queryByTestId('system-install-refused')).toBeNull();
  });

  it('an install that fails before the restart shows why', async () => {
    const failedJob: Job = {
      ...systemInstallRequesting,
      state: 'failed',
      phase: 'preparing',
      error: { code: 'internal_error', message: 'slot 2 no longer matches the upload', request_id: 'req_2', retryable: false },
    };
    systemDevice({ extra: { 'GET /api/v1/jobs/job_00000201': () => json(200, failedJob) } });
    render(<App />);
    await uploadAndVerify();
    await waitFor(() => expect(installButton()).toBeEnabled());
    await userEvent.click(installButton());
    await userEvent.click(await screen.findByRole('button', { name: t('sysupd.confirm_submit') }));
    const failed = await screen.findByTestId('system-install-failed', {}, { timeout: 5_000 });
    expect(failed).toHaveTextContent(t('error.internal_error'));
    expect(failed).toHaveTextContent('slot 2');
    expect(screen.getByTestId('system-phase-preparing')).toHaveAttribute('data-status', 'stopped');
  });

  it('the restart ends the session: the sign-in screen says so, and the path stays on this screen', async () => {
    systemDevice({ afterReboot: () => refusal(401, 'authentication_required') });
    render(<App />);
    await uploadAndVerify();
    await waitFor(() => expect(installButton()).toBeEnabled());
    await userEvent.click(installButton());
    await userEvent.click(await screen.findByRole('button', { name: t('sysupd.confirm_submit') }));
    expect(await screen.findByText(t('login.session_ended'), {}, { timeout: 10_000 })).toBeInTheDocument();
    expect(window.location.pathname).toBe('/firmware');
    expect(window.localStorage.getItem(SYSTEM_STORAGE_KEY)).toBeNull();
  });
});

describe('after a reload', () => {
  it('shows a remembered verified upload without a file chosen', async () => {
    systemDevice({ upload: systemUploadReady });
    window.localStorage.setItem(SYSTEM_STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0002' }));
    render(<App />);
    expect(await screen.findByTestId('system-image-version')).toHaveTextContent('1.1.0+0');
    expect(screen.getByTestId('system-upload-state')).toHaveTextContent(t('update.upload_state.ready'));
  });

  it('continues a remembered upload from what the device received', async () => {
    const bytes = image();
    const dev = systemDevice({ upload: { ...systemUploadReceiving, received_bytes: 16384, sha256: sha(bytes) } });
    window.localStorage.setItem(SYSTEM_STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0002' }));
    render(<App />);
    expect(await screen.findByTestId('system-upload-progress')).toHaveTextContent('16384');
    await chooseFile(bytes);
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_resume') }));
    await screen.findByTestId('system-image-version', {}, { timeout: 10_000 });
    expect(dev.creates).toEqual([]);
    expect(dev.chunks.map((c) => c.offset)).toEqual([16384, 32768]);
  });

  it('does not take an ESP32 upload for its own', async () => {
    systemDevice({ other: uploadReady });
    window.localStorage.setItem(SYSTEM_STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0001' }));
    render(<App />);
    expect(await screen.findByTestId('other-upload')).toHaveTextContent(t('update.target.esp32c6'));
    expect(screen.queryByTestId('system-upload-state')).toBeNull();
    await waitFor(() => expect(window.localStorage.getItem(SYSTEM_STORAGE_KEY)).toBeNull());
  });

  it('without the device list (older firmware) it still refuses an ESP32 upload it remembered', async () => {
    systemDevice({
      extra: {
        'GET /api/v1/firmware/uploads': () => refusal(404, 'not_found'),
        'GET /api/v1/firmware/uploads/upload_0001': () => json(200, uploadReady),
      },
    });
    window.localStorage.setItem(SYSTEM_STORAGE_KEY, JSON.stringify({ uploadId: 'upload_0001' }));
    render(<App />);
    expect(await screen.findByTestId('system-upload-notice')).toHaveTextContent(t('sysupd.wrong_target'));
    expect(screen.queryByTestId('system-upload-state')).toBeNull();
  });

  it('shows an upload another browser left, without a remembered id', async () => {
    systemDevice({ upload: systemUploadReady });
    render(<App />);
    expect(await screen.findByTestId('system-image-version')).toHaveTextContent('1.1.0+0');
    expect(JSON.parse(window.localStorage.getItem(SYSTEM_STORAGE_KEY)!)).toMatchObject({ uploadId: 'upload_0002' });
  });

  it('an unfinished upload another browser left asks for the same file, then continues it', async () => {
    const bytes = image();
    const dev = systemDevice({ upload: { ...systemUploadReceiving, received_bytes: 16384, sha256: sha(bytes) } });
    render(<App />);
    expect(await screen.findByTestId('system-upload-continue')).toHaveTextContent(String(SIZE));
    await chooseFile(bytes);
    expect(screen.queryByTestId('system-upload-continue')).toBeNull();
    await userEvent.click(screen.getByRole('button', { name: t('update.upload_resume') }));
    await screen.findByTestId('system-image-version', {}, { timeout: 10_000 });
    expect(dev.creates).toEqual([]);
    expect(dev.chunks.map((c) => c.offset)).toEqual([16384, 32768]);
  });

  it('an install job the device no longer knows means it restarted', async () => {
    systemDevice({ extra: { 'GET /api/v1/jobs/job_00000201': () => refusal(404, 'not_found') } });
    window.localStorage.setItem(SYSTEM_STORAGE_KEY, JSON.stringify({ installJobId: 'job_00000201' }));
    render(<App />);
    expect(await screen.findByTestId('system-install-notice')).toHaveTextContent(t('sysupd.job_gone'));
    expect(window.localStorage.getItem(SYSTEM_STORAGE_KEY)).toBeNull();
  });

  it('an install still in rebooting is followed to the restart', async () => {
    // The job already answers rebooting; the first status is still the old boot, the next the new one.
    let polls = 0;
    window.localStorage.setItem(SYSTEM_STORAGE_KEY, JSON.stringify({ installJobId: 'job_00000201' }));
    systemDevice({
      extra: {
        'GET /api/v1/jobs/job_00000201': () => json(200, systemInstallRebooting),
        'GET /api/v1/system/status': () => json(200, { ...systemStatus, boot_id: polls++ < 1 ? BOOT_BEFORE : BOOT_AFTER }),
      },
    });
    render(<App />);
    expect(await screen.findByTestId('system-install-notice', {}, { timeout: 10_000 })).toHaveTextContent(t('sysupd.rebooted'));
  });
});

describe('navigation', () => {
  it('the overview links the firmware version to this screen', async () => {
    window.history.replaceState(null, '', '/');
    systemDevice({
      extra: {
        'GET /api/v1/network/status': () => refusal(404, 'not_found'),
        'GET /api/v1/matter/status': () => refusal(404, 'not_found'),
        'GET /api/v1/coprocessor/status': () => refusal(404, 'not_found'),
      },
    });
    render(<App />);
    await userEvent.click(await screen.findByTestId('overview-firmware-link'));
    expect(await screen.findByRole('heading', { level: 1, name: t('sysupd.title') })).toBeInTheDocument();
    expect(window.location.pathname).toBe('/firmware');
  });
});
