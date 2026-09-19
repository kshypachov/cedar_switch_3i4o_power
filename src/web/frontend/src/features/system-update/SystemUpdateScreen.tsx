import { useCallback, useEffect, useRef, useState } from 'react';

import { getCapabilities, getJob, getSystemStatus } from '../../api/device';
import { isApiFailure } from '../../api/errors';
import {
  cancelJob,
  createUpload,
  deleteUpload,
  getUpload,
  newIdempotencyKey,
  verifyUpload,
  writeUploadChunk,
} from '../../api/firmware';
import { pollJob } from '../../api/jobs';
import { getSystemFirmware, startSystemUpdate } from '../../api/system';
import type { Job, Session, Upload } from '../../api/types';
import { ErrorNotice } from '../../components/ErrorNotice';
import { Body } from '../../components/Polled';
import { Card, Facts } from '../../components/ui';
import { type MessageKey, t } from '../../i18n';
import { useAuth } from '../../state/auth';
import { usePolling } from '../../state/usePolling';
import { notServed, useSessionGuard } from '../../state/useSessionGuard';
import { findDeviceUploads, OtherUpload } from '../coprocessor-update/deviceUploads';
import { phaseRowsOf } from '../coprocessor-update/phases';
import { memoryAt } from '../coprocessor-update/remember';
import { hashBlob } from '../coprocessor-update/sha256';
import { DetailText } from '../coprocessor-update/StatusCard';
import { sendChunks, UploadStopped } from '../coprocessor-update/uploader';
import { FirmwareCard, LastUpdateCard, unavailableReason } from './FirmwareCard';
import { followSystemInstall, SYSTEM_PHASES, waitForReboot } from './install';
import { compareVersions } from './version';

const FIRMWARE_MS = 2_000;
const CAPABILITIES_MS = 10_000;
const FIRMWARE_POLL_MS = 500;
const DEFAULT_CHUNK_BYTES = 16_384;
const FILENAME_MAX = 128;
const TARGET = 'stm32u585';

/** The STM32 screen's own entry: an ESP32 upload in the other screen is not forgotten. */
export const SYSTEM_STORAGE_KEY = 'cedar.system-update';
const memory = memoryAt(SYSTEM_STORAGE_KEY);

type Busy = 'hashing' | 'sending' | 'verifying' | 'deleting' | null;
type Stage =
  | { kind: 'idle' }
  | { kind: 'confirming' }
  | { kind: 'following' }
  | { kind: 'rebooting'; elapsedMs: number }
  | { kind: 'back' }
  | { kind: 'timeout'; elapsedMs: number; bootId: string };

/** Thrown out of a wait when the page went away or the session ended. */
class Stop extends Error {}

const sleep = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

function clipFilename(name: string): string {
  const clipped = [...name].slice(0, FILENAME_MAX).join('');
  return clipped.length > 0 ? clipped : 'zephyr.signed.bin';
}

function Phases({ job }: { job: Job | null }) {
  return (
    <ol className="phases" aria-label={t('sysupd.install_running')}>
      {phaseRowsOf(SYSTEM_PHASES, job).map((row) => (
        <li key={row.phase} className={`phase phase-${row.status}`} data-testid={`system-phase-${row.phase}`} data-status={row.status}>
          <span>{t(`sysupd.phase.${row.phase}`)}</span>
          <span>{t(`update.phase_status.${row.status}`)}</span>
        </li>
      ))}
    </ol>
  );
}

/** A refusal of startSystemUpdate in words; null when the generic notice has to do. */
function installRefusal(error: unknown, runningConfirmed: boolean | undefined, swapPending: boolean | undefined): MessageKey | null {
  if (isApiFailure(error, 'busy')) return 'sysupd.busy';
  if (isApiFailure(error, 'invalid_state')) {
    return runningConfirmed === false || swapPending ? 'sysupd.unconfirmed' : 'sysupd.not_ready';
  }
  if (isApiFailure(error, 'validation_failed')) return 'sysupd.downgrade_required';
  if (isApiFailure(error, 'unsupported_target')) return 'sysupd.unsupported_target';
  return null;
}

export function SystemUpdateScreen({ session }: { session: Session }) {
  const { signedOut } = useAuth();
  const firmware = usePolling(getSystemFirmware, FIRMWARE_MS, { stopOn: notServed });
  const capabilities = usePolling(getCapabilities, CAPABILITIES_MS, { stopOn: notServed });
  useSessionGuard(firmware, capabilities);

  const [file, setFile] = useState<File | null>(null);
  const [fileProblem, setFileProblem] = useState<MessageKey | null>(null);
  const [hashed, setHashed] = useState<{ done: number; total: number } | null>(null);
  const [sha256, setSha256] = useState<string | null>(null);
  const [upload, setUpload] = useState<Upload | null>(null);
  const [busy, setBusy] = useState<Busy>(null);
  const [uploadError, setUploadError] = useState<unknown>(null);
  const [uploadNotice, setUploadNotice] = useState<MessageKey | null>(null);
  /** The ESP32's upload, which keeps this screen from uploading until it is gone. */
  const [other, setOther] = useState<Upload | null>(null);

  const [acknowledgeDowngrade, setAcknowledgeDowngrade] = useState(false);
  const [stage, setStage] = useState<Stage>({ kind: 'idle' });
  const [installJob, setInstallJob] = useState<Job | null>(null);
  const [installError, setInstallError] = useState<unknown>(null);
  const [installNotice, setInstallNotice] = useState<MessageKey | null>(null);
  const [connectionLost, setConnectionLost] = useState(false);

  const keys = useRef(new Map<string, string>());
  const keyFor = (action: string): string => {
    let key = keys.current.get(action);
    if (!key) {
      key = newIdempotencyKey();
      keys.current.set(action, key);
    }
    return key;
  };
  const alive = useRef(new AbortController());
  useEffect(() => {
    const controller = alive.current;
    return () => controller.abort();
  }, []);
  const signal = alive.current.signal;

  const waitJob = useCallback(
    async (jobId: string): Promise<Job> => {
      const outcome = await pollJob(jobId, {
        signal,
        intervalMs: FIRMWARE_POLL_MS,
        onUpdate: () => setConnectionLost(false),
        onTransient: () => setConnectionLost(true),
      });
      setConnectionLost(false);
      if (outcome.kind === 'session_ended') {
        signedOut('session_ended');
        throw new Stop();
      }
      return outcome.job;
    },
    [signal, signedOut],
  );

  const awaitReboot = useCallback(
    async (bootId: string) => {
      setStage({ kind: 'rebooting', elapsedMs: 0 });
      const outcome = await waitForReboot(
        {
          getStatus: () => getSystemStatus(signal),
          sleep,
          now: () => Date.now(),
          onWaiting: (elapsedMs) => {
            if (!signal.aborted) setStage({ kind: 'rebooting', elapsedMs });
          },
        },
        bootId,
      );
      if (signal.aborted) return;
      if (outcome.kind === 'timeout') {
        setStage({ kind: 'timeout', elapsedMs: outcome.elapsedMs, bootId });
        return;
      }
      memory.remember({ installJobId: null });
      if (outcome.kind === 'session_ended') {
        // The restart ended every session; the path stays /firmware, so the
        // result is on this screen right after signing in again.
        signedOut('session_ended');
        return;
      }
      setStage({ kind: 'back' });
      setInstallNotice('sysupd.rebooted');
    },
    [signal, signedOut],
  );

  const followInstall = useCallback(
    async (jobId: string, bootIdBefore: string | null) => {
      setStage({ kind: 'following' });
      setInstallError(null);
      try {
        const outcome = await followSystemInstall(
          {
            getJob: (id) => getJob(id, signal),
            sleep,
            onUpdate: (job) => {
              if (signal.aborted) return;
              setConnectionLost(false);
              setInstallJob(job);
            },
            onTransient: () => setConnectionLost(true),
          },
          jobId,
        );
        if (signal.aborted) return;
        setConnectionLost(false);
        switch (outcome.kind) {
          case 'finished':
            memory.remember({ installJobId: null });
            setInstallJob(outcome.job);
            setInstallNotice(outcome.job.state === 'cancelled' ? 'sysupd.install_cancelled' : null);
            setStage({ kind: 'idle' });
            return;
          case 'gone':
            memory.remember({ installJobId: null });
            setInstallNotice('sysupd.job_gone');
            setStage({ kind: 'idle' });
            return;
          case 'session_ended':
            memory.remember({ installJobId: null });
            signedOut('session_ended');
            return;
          case 'rebooting': {
            const bootId = outcome.job?.boot_id ?? bootIdBefore;
            if (!bootId) {
              memory.remember({ installJobId: null });
              setInstallNotice('sysupd.job_gone');
              setStage({ kind: 'idle' });
              return;
            }
            await awaitReboot(bootId);
            return;
          }
        }
      } catch (error) {
        if (signal.aborted) return;
        setInstallError(error);
        setStage({ kind: 'idle' });
      }
    },
    [signal, signedOut, awaitReboot],
  );

  /**
   * The device is the authority on which upload exists: this screen's own (from
   * another window, a script, a reload) and the ESP32's. The remembered id only
   * says which one this browser was sending.
   */
  const discover = useCallback(async () => {
    const found = await findDeviceUploads(TARGET, signal);
    if (!signal.aborted) {
      setOther(found.other);
      memory.remember({ uploadId: found.own?.id ?? null });
      setUpload(found.own);
    }
    return found;
  }, [signal]);

  // Pick up what a reload interrupted - or what someone else left on the device.
  useEffect(() => {
    const { uploadId, installJobId } = memory.recall();
    discover()
      .then(async (found) => {
        const own = found.own;
        if (own?.state === 'verifying' && own.active_job_id && !signal.aborted) {
          // A check started elsewhere: its result shows here when it ends.
          setBusy('verifying');
          try {
            await waitJob(own.active_job_id);
            if (!signal.aborted) setUpload(await getUpload(own.id, signal));
          } catch {
            /* the upload card keeps the last state it knew */
          } finally {
            if (!signal.aborted) setBusy(null);
          }
        }
      })
      .catch(() => {
        // A device without the list (older firmware): the remembered id only.
        if (!uploadId || signal.aborted) return;
        getUpload(uploadId, signal)
          .then((u) => {
            if (signal.aborted) return;
            if (u.target !== TARGET) {
              memory.remember({ uploadId: null });
              setUploadNotice('sysupd.wrong_target');
              return;
            }
            setUpload(u);
          })
          .catch((error: unknown) => {
            if (isApiFailure(error, 'not_found')) memory.remember({ uploadId: null });
          });
      });
    if (installJobId) void followInstall(installJobId, null);
  }, [signal, followInstall, discover, waitJob]);

  const limits = capabilities.data?.limits;
  const maxBytes = limits?.system_upload_max_bytes;

  async function choose(chosen: File | null) {
    setFile(chosen);
    setSha256(null);
    setHashed(null);
    setFileProblem(null);
    setUploadError(null);
    setUploadNotice(null);
    if (!chosen) return;
    if (chosen.size === 0) {
      setFileProblem('update.file_empty');
      return;
    }
    if (maxBytes !== undefined && chosen.size > maxBytes) {
      setFileProblem('sysupd.file_too_large');
      return;
    }
    setBusy('hashing');
    try {
      const digest = await hashBlob(chosen, { signal, onProgress: (done, total) => setHashed({ done, total }) });
      if (!signal.aborted) setSha256(digest);
    } catch (error) {
      if (!signal.aborted) setUploadError(error);
    } finally {
      if (!signal.aborted) setBusy(null);
    }
  }

  async function verify(current: Upload): Promise<void> {
    setBusy('verifying');
    const action = `verify:${current.id}`;
    const accepted = await verifyUpload(session.csrf_token, keyFor(action), current.id);
    await waitJob(accepted.job_id);
    keys.current.delete(action);
    setUpload(await getUpload(current.id, signal));
  }

  async function send() {
    if (!file || !sha256) return;
    setUploadError(null);
    setUploadNotice(null);
    setAcknowledgeDowngrade(false);
    setBusy('sending');
    try {
      let current = upload;
      const same = current !== null && current.size_bytes === file.size && current.sha256 === sha256;
      if (current && !same && current.state !== 'failed') {
        setUploadNotice('update.upload_mismatch');
        return;
      }
      if (!current || current.state === 'failed') {
        const action = `create:${sha256}:${file.size}`;
        try {
          current = await createUpload(session.csrf_token, keyFor(action), {
            filename: clipFilename(file.name),
            size_bytes: file.size,
            sha256,
            target: TARGET,
          });
        } catch (error) {
          if (!isApiFailure(error, 'busy')) {
            if (isApiFailure(error, 'invalid_state')) {
              setUploadNotice('sysupd.upload_unconfirmed');
              return;
            }
            if (isApiFailure(error, 'payload_too_large')) {
              setFileProblem('sysupd.file_too_large');
              return;
            }
            throw error;
          }
          // Busy: the device holds an upload this page did not know about. Show it;
          // the same file continues it, anything else has to delete it first.
          const found = await discover();
          const own = found.own;
          if (!own || own.size_bytes !== file.size || own.sha256 !== sha256 || own.state === 'failed') {
            setUploadNotice(own ? 'update.upload_found' : found.other ? null : 'update.upload_busy_unknown');
            return;
          }
          current = own;
        }
        keys.current.delete(action);
        memory.remember({ uploadId: current.id });
        setUpload(current);
      }
      if (current.state === 'verifying' && current.active_job_id) {
        await waitJob(current.active_job_id);
        setUpload(await getUpload(current.id, signal));
        return;
      }
      if (current.state === 'receiving') {
        const chosen = file;
        current = await sendChunks(
          {
            getUpload: (id) => getUpload(id, signal),
            putChunk: (id, offset, bytes, key) => writeUploadChunk(session.csrf_token, key, id, offset, bytes, signal),
            waitJob: (id) => waitJob(id),
            read: async (offset, length) => new Uint8Array(await chosen.slice(offset, offset + length).arrayBuffer()),
            newKey: newIdempotencyKey,
            sleep,
            onProgress: (u) => {
              if (!signal.aborted) setUpload(u);
            },
          },
          current,
          limits?.upload_chunk_bytes ?? DEFAULT_CHUNK_BYTES,
        );
        setUpload(current);
        await verify(current);
      }
    } catch (error) {
      if (error instanceof Stop || signal.aborted) return;
      setUploadError(error);
      const id = error instanceof UploadStopped ? error.upload.id : upload?.id;
      if (id) setUpload(await getUpload(id, signal).catch(() => null));
    } finally {
      if (!signal.aborted) setBusy(null);
    }
  }

  async function reverify() {
    if (!upload) return;
    setUploadError(null);
    try {
      await verify(upload);
    } catch (error) {
      if (!(error instanceof Stop) && !signal.aborted) setUploadError(error);
    } finally {
      if (!signal.aborted) setBusy(null);
    }
  }

  async function remove() {
    if (!upload) return;
    setUploadError(null);
    setUploadNotice(null);
    setBusy('deleting');
    const action = `delete:${upload.id}`;
    try {
      const accepted = await deleteUpload(session.csrf_token, keyFor(action), upload.id);
      await waitJob(accepted.job_id);
      keys.current.delete(action);
      memory.remember({ uploadId: null });
      setUpload(null);
      setAcknowledgeDowngrade(false);
    } catch (error) {
      if (isApiFailure(error, 'not_found')) {
        memory.remember({ uploadId: null });
        setUpload(null);
      } else if (!(error instanceof Stop) && !signal.aborted) {
        setUploadError(error);
      }
    } finally {
      if (!signal.aborted) setBusy(null);
    }
  }

  /** Delete the ESP32's upload that keeps this screen from uploading. */
  async function removeOther() {
    if (!other) return;
    setUploadError(null);
    setUploadNotice(null);
    setBusy('deleting');
    const action = `delete:${other.id}`;
    try {
      const accepted = await deleteUpload(session.csrf_token, keyFor(action), other.id);
      await waitJob(accepted.job_id);
      keys.current.delete(action);
    } catch (error) {
      if (!isApiFailure(error, 'not_found') && !(error instanceof Stop) && !signal.aborted) {
        setUploadError(error);
      }
    } finally {
      if (!signal.aborted) {
        await discover().catch(() => setOther(null));
        setBusy(null);
      }
    }
  }

  const fw = firmware.data;
  const image = upload?.state === 'ready' && upload.target === TARGET ? upload.image : null;
  const comparison = image && fw ? compareVersions(image.version, fw.running.version) : 'unknown';
  const unavailable = unavailableReason(fw, capabilities.data?.features.stm32_update);
  const following = stage.kind === 'following' || stage.kind === 'rebooting' || stage.kind === 'timeout';
  const blocker: MessageKey | null = !image
    ? 'sysupd.install_needs_ready'
    : fw && !fw.running.confirmed
      ? 'sysupd.install_needs_confirmed'
      : fw?.swap_pending
        ? 'sysupd.install_swap_pending'
        : null;
  /** Everything the request needs; the confirmation panel is the last step before it. */
  const canRequest =
    image !== null &&
    fw !== null &&
    unavailable === null &&
    blocker === null &&
    (comparison !== 'older' || acknowledgeDowngrade) &&
    busy === null &&
    !following;
  const canInstall = canRequest && stage.kind !== 'confirming';
  // A confirmation asked for an install the device no longer allows is dropped, not kept for later.
  useEffect(() => {
    if (stage.kind === 'confirming' && !canRequest) setStage({ kind: 'idle' });
  }, [stage.kind, canRequest]);

  async function install() {
    if (!upload || !canRequest) return;
    setInstallError(null);
    setInstallNotice(null);
    setInstallJob(null);
    // The panel closes at once: a second press must not send a second request.
    setStage({ kind: 'following' });
    const action = `install:${upload.id}:${acknowledgeDowngrade}`;
    // The boot this page saw before the request: the restart is a different one.
    const before = await getSystemStatus(signal).catch(() => null);
    let jobId: string;
    try {
      jobId = (await startSystemUpdate(session.csrf_token, keyFor(action), upload.id, comparison === 'older' && acknowledgeDowngrade)).job_id;
    } catch (error) {
      if (!signal.aborted) {
        setInstallError(error);
        setStage({ kind: 'idle' });
      }
      return;
    }
    keys.current.delete(action);
    memory.remember({ installJobId: jobId, uploadId: null });
    setAcknowledgeDowngrade(false);
    await followInstall(jobId, before?.boot_id ?? null);
  }

  async function cancel() {
    if (!installJob) return;
    const action = `cancel:${installJob.id}`;
    try {
      await cancelJob(session.csrf_token, keyFor(action), installJob.id);
      keys.current.delete(action);
    } catch (error) {
      if (isApiFailure(error, 'invalid_state')) {
        keys.current.delete(action);
        setInstallNotice('sysupd.cancel_refused');
      } else if (!signal.aborted) {
        setInstallError(error);
      }
    }
  }

  const resumable =
    upload !== null && file !== null && upload.state === 'receiving' && upload.size_bytes === file.size && upload.sha256 === sha256;
  const refusal = installRefusal(installError, fw?.running.confirmed, fw?.swap_pending);

  return (
    <main>
      <h1>{t('sysupd.title')}</h1>
      <Body polled={firmware} render={(f) => <FirmwareCard firmware={f} />} />
      <Body polled={firmware} render={(f) => <LastUpdateCard firmware={f} />} />

      <Card title="sysupd.file">
        <p>{t('sysupd.file_intro')}</p>
        <div className="field">
          <label htmlFor="system-firmware-file">{t('sysupd.file_label')}</label>
          <input
            id="system-firmware-file"
            type="file"
            accept=".bin,application/octet-stream"
            disabled={busy !== null || following}
            onChange={(e) => void choose(e.target.files?.[0] ?? null)}
          />
        </div>
        {fileProblem ? (
          <p className="field-error" role="alert" data-testid="system-file-problem">
            {t(fileProblem, { max: maxBytes ?? 0 })}
          </p>
        ) : null}
        {busy === 'hashing' && hashed ? (
          <p role="status" className="muted">
            {t('update.hashing', { percent: Math.floor((hashed.done * 100) / Math.max(1, hashed.total)) })}
          </p>
        ) : null}
        {file && sha256 ? (
          <Facts
            rows={[
              ['update.size', t('update.bytes', { n: file.size })],
              ['update.sha256', <code key="sha" data-testid="system-file-sha256">{sha256}</code>],
            ]}
          />
        ) : null}
      </Card>

      <Card title="update.upload">
        {other ? <OtherUpload upload={other} disabled={busy !== null || following} onDelete={() => void removeOther()} /> : null}
        {upload ? (
          <>
            <Facts
              rows={[
                ['update.upload_file', upload.filename],
                ['update.upload_state', <span key="state" data-testid="system-upload-state">{t(`update.upload_state.${upload.state}`)}</span>],
                ['update.size', t('update.bytes', { n: upload.size_bytes })],
              ]}
            />
            {upload.state === 'receiving' ? (
              <>
                <p className="muted" data-testid="system-upload-progress">
                  {t('update.uploading', { received: upload.received_bytes, total: upload.size_bytes })}
                </p>
                <progress className="upload-progress" value={upload.received_bytes} max={upload.size_bytes} />
                {!resumable && busy === null && upload.received_bytes < upload.size_bytes ? (
                  <p className="muted" data-testid="system-upload-continue">
                    {t('update.upload_continue_hint', { size: upload.size_bytes })}
                  </p>
                ) : null}
              </>
            ) : null}
            {upload.state === 'failed' && upload.error ? (
              <p className="notice notice-error" role="alert" data-testid="system-verify-failed">
                {t('update.verify_failed', { reason: '' })}
                <DetailText detail={upload.error} />
              </p>
            ) : null}
            {image ? (
              <>
                <h3>{t('sysupd.image')}</h3>
                <Facts
                  rows={[
                    ['update.image_format', t(`update.format.${image.format}`)],
                    ['sysupd.image_version', <code key="version" data-testid="system-image-version">{image.version}</code>],
                    [
                      'sysupd.image_compare',
                      <span key="compare" data-testid="system-image-compare" data-comparison={comparison}>
                        {t(`sysupd.compare.${comparison}`, { running: fw?.running.version ?? t('value.unknown') })}
                      </span>,
                    ],
                  ]}
                />
              </>
            ) : null}
          </>
        ) : null}
        {busy === 'verifying' ? (
          <p role="status" className="muted">
            {t('update.verifying')}
          </p>
        ) : null}
        {busy === 'deleting' ? (
          <p role="status" className="muted">
            {t('update.deleting')}
          </p>
        ) : null}
        {uploadNotice ? (
          <p className="notice notice-warning" role="alert" data-testid="system-upload-notice">
            {t(uploadNotice)}
          </p>
        ) : null}
        {uploadError instanceof UploadStopped && uploadError.job?.error ? (
          <p className="notice notice-error" role="alert">
            {t('update.upload_stopped', { reason: '' })}
            <DetailText detail={uploadError.job.error} />
          </p>
        ) : uploadError ? (
          <ErrorNotice error={uploadError} />
        ) : null}
        <div className="actions">
          <button type="button" disabled={!file || !sha256 || busy !== null || following} onClick={() => void send()}>
            {t(resumable && upload && upload.received_bytes > 0 ? 'update.upload_resume' : 'update.upload_submit')}
          </button>
          {upload && upload.state === 'receiving' && upload.received_bytes === upload.size_bytes ? (
            <button type="button" className="button-secondary" disabled={busy !== null} onClick={() => void reverify()}>
              {t('update.verify_submit')}
            </button>
          ) : null}
          {upload ? (
            <button type="button" className="button-secondary" disabled={busy !== null || following} onClick={() => void remove()}>
              {t('update.delete_submit')}
            </button>
          ) : null}
        </div>
      </Card>

      <Card title="sysupd.install">
        <p className="muted">{t('sysupd.install_intro')}</p>
        {unavailable !== null ? (
          <p className="muted" data-testid="system-install-unavailable">
            {t('sysupd.install_unavailable', { reason: unavailable })}
          </p>
        ) : null}
        {blocker && !following ? (
          <p className="muted" data-testid="system-install-blocker">
            {t(blocker)}
          </p>
        ) : null}
        {image && comparison === 'older' && !following ? (
          <div className="field field-check">
            <label>
              <input type="checkbox" checked={acknowledgeDowngrade} onChange={(e) => setAcknowledgeDowngrade(e.target.checked)} />
              {t('sysupd.downgrade_acknowledge')}
            </label>
          </div>
        ) : null}
        {/* The panel stays only while the request is still allowed: a poll that finds the
            firmware unconfirmed or a swap pending (another browser installed) takes it away. */}
        {stage.kind === 'confirming' && image && canRequest ? (
          <div className="notice notice-warning" role="group" aria-label={t('sysupd.confirm_title', { version: image.version })} data-testid="system-confirm">
            <h3>{t('sysupd.confirm_title', { version: image.version })}</h3>
            <p>{t('sysupd.confirm_text')}</p>
            <div className="actions">
              <button type="button" onClick={() => void install()}>
                {t('sysupd.confirm_submit')}
              </button>
              <button type="button" className="button-secondary" onClick={() => setStage({ kind: 'idle' })}>
                {t('sysupd.confirm_cancel')}
              </button>
            </div>
          </div>
        ) : (
          <div className="actions">
            <button type="button" disabled={!canInstall} onClick={() => setStage({ kind: 'confirming' })}>
              {t('sysupd.install_submit')}
            </button>
            {stage.kind === 'following' && installJob?.cancellable ? (
              <button type="button" className="button-secondary" onClick={() => void cancel()}>
                {t('sysupd.cancel_submit')}
              </button>
            ) : null}
          </div>
        )}
        {following || installJob ? <Phases job={installJob} /> : null}
        {connectionLost && stage.kind === 'following' ? (
          <p role="status" className="notice">
            {t('update.connection_lost')}
          </p>
        ) : null}
        {stage.kind === 'rebooting' ? (
          <p role="status" className="notice" data-testid="system-rebooting">
            {t('sysupd.rebooting', { seconds: Math.floor(stage.elapsedMs / 1000) })}
          </p>
        ) : null}
        {stage.kind === 'timeout' ? (
          <div role="alert" className="notice notice-error" data-testid="system-reboot-timeout">
            <p>{t('sysupd.reboot_timeout', { seconds: Math.floor(stage.elapsedMs / 1000) })}</p>
            <button type="button" onClick={() => void awaitReboot(stage.bootId)}>
              {t('sysupd.reboot_retry')}
            </button>
          </div>
        ) : null}
        {installNotice ? (
          <p role="status" className="notice" data-testid="system-install-notice">
            {t(installNotice)}
          </p>
        ) : null}
        {installJob && stage.kind === 'idle' && installJob.state === 'failed' && installJob.error ? (
          <p role="alert" className="notice notice-error" data-testid="system-install-failed">
            {t('sysupd.install_failed', { reason: '' })}
            <DetailText detail={installJob.error} />
          </p>
        ) : null}
        {refusal ? (
          <p role="alert" className="notice notice-error" data-testid="system-install-refused">
            {t(refusal)}
          </p>
        ) : installError ? (
          <ErrorNotice error={installError} />
        ) : null}
      </Card>
    </main>
  );
}
