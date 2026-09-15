import { useCallback, useEffect, useRef, useState } from 'react';

import { getCapabilities, getCoprocessorStatus } from '../../api/device';
import { isApiFailure } from '../../api/errors';
import {
  cancelJob,
  createUpload,
  deleteUpload,
  getUpload,
  newIdempotencyKey,
  startCoprocessorUpdate,
  verifyUpload,
  writeUploadChunk,
} from '../../api/firmware';
import { pollJob } from '../../api/jobs';
import type { Job, Session, Upload } from '../../api/types';
import { ErrorNotice } from '../../components/ErrorNotice';
import { Body } from '../../components/Polled';
import { Card, Facts } from '../../components/ui';
import { type MessageKey, t } from '../../i18n';
import { useAuth } from '../../state/auth';
import { usePolling } from '../../state/usePolling';
import { notServed, useSessionGuard } from '../../state/useSessionGuard';
import { phaseRows, type PhaseRow } from './phases';
import { recall, remember } from './remember';
import { hashBlob } from './sha256';
import { DetailText, reasonText, StatusCard } from './StatusCard';
import { sendChunks, UploadStopped } from './uploader';

const STATUS_MS = 2_000;
const CAPABILITIES_MS = 10_000;
/** Chunk and verify jobs: the short end of the contract's 500-1000 ms. */
const FIRMWARE_POLL_MS = 500;
const DEFAULT_CHUNK_BYTES = 16_384;
const FILENAME_MAX = 128;

type Busy = 'hashing' | 'sending' | 'verifying' | 'deleting' | null;

/** Thrown out of a wait when the page went away or the session ended. */
class Stop extends Error {}

const sleep = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

function progressText(row: PhaseRow): string | null {
  const p = row.progress;
  if (!p || p.total === null) return null;
  return t(p.unit === 'bytes' ? 'update.progress_bytes' : 'update.progress_steps', { completed: p.completed, total: p.total });
}

function Phases({ job }: { job: Job | null }) {
  return (
    <ol className="phases" aria-label={t('update.install_running')}>
      {phaseRows(job).map((row) => {
        const progress = progressText(row);
        return (
          <li key={row.phase} className={`phase phase-${row.status}`} data-testid={`phase-${row.phase}`} data-status={row.status}>
            <span>{t(`update.phase.${row.phase}`)}</span>
            <span>
              {t(`update.phase_status.${row.status}`)}
              {progress ? <span className="small">{progress}</span> : null}
            </span>
            {row.status === 'current' && row.progress && row.progress.total ? (
              <progress value={row.progress.completed} max={row.progress.total} />
            ) : null}
          </li>
        );
      })}
    </ol>
  );
}

function clipFilename(name: string): string {
  const clipped = [...name].slice(0, FILENAME_MAX).join('');
  return clipped.length > 0 ? clipped : 'firmware.bin';
}

export function CoprocessorUpdateScreen({ session }: { session: Session }) {
  const { signedOut } = useAuth();
  const status = usePolling(getCoprocessorStatus, STATUS_MS, { stopOn: notServed });
  const capabilities = usePolling(getCapabilities, CAPABILITIES_MS, { stopOn: notServed });
  useSessionGuard(status, capabilities);

  const [file, setFile] = useState<File | null>(null);
  const [fileProblem, setFileProblem] = useState<MessageKey | null>(null);
  const [hashed, setHashed] = useState<{ done: number; total: number } | null>(null);
  const [sha256, setSha256] = useState<string | null>(null);
  const [upload, setUpload] = useState<Upload | null>(null);
  const [busy, setBusy] = useState<Busy>(null);
  const [uploadError, setUploadError] = useState<unknown>(null);
  const [uploadNotice, setUploadNotice] = useState<MessageKey | null>(null);

  const [acknowledged, setAcknowledged] = useState(false);
  const [installJob, setInstallJob] = useState<Job | null>(null);
  const [installing, setInstalling] = useState(false);
  const [installError, setInstallError] = useState<unknown>(null);
  const [installNotice, setInstallNotice] = useState<MessageKey | null>(null);
  const [connectionLost, setConnectionLost] = useState(false);
  const [cancelRefused, setCancelRefused] = useState(false);

  // One Idempotency-Key per intended action, kept for its retries and dropped
  // once the action has its answer (see api/firmware.ts).
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
    async (jobId: string, onUpdate?: (job: Job) => void, intervalMs = FIRMWARE_POLL_MS): Promise<Job> => {
      const outcome = await pollJob(jobId, {
        signal,
        intervalMs,
        onUpdate: (job) => {
          setConnectionLost(false);
          onUpdate?.(job);
        },
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

  const followInstall = useCallback(
    async (jobId: string) => {
      setInstalling(true);
      setInstallError(null);
      setInstallNotice(null);
      try {
        const job = await waitJob(jobId, setInstallJob);
        setInstallJob(job);
        remember({ installJobId: null });
        setInstallNotice(
          job.state === 'succeeded'
            ? 'update.install_succeeded'
            : job.state === 'cancelled'
              ? 'update.install_cancelled'
              : null,
        );
      } catch (error) {
        if (error instanceof Stop || signal.aborted) return;
        if (isApiFailure(error, 'not_found')) {
          // A job is RAM on the device: after a reboot it is gone, and the
          // outcome is the journal's last_update in the status.
          remember({ installJobId: null });
          setInstallNotice('update.job_gone');
        } else {
          setInstallError(error);
        }
      } finally {
        if (!signal.aborted) setInstalling(false);
      }
    },
    [signal, waitJob],
  );

  // Pick up what a reload interrupted: the upload being sent, the install being followed.
  useEffect(() => {
    const { uploadId, installJobId } = recall();
    if (uploadId) {
      getUpload(uploadId, signal)
        .then((u) => {
          if (!signal.aborted) setUpload(u);
        })
        .catch((error: unknown) => {
          if (isApiFailure(error, 'not_found')) remember({ uploadId: null });
        });
    }
    if (installJobId) void followInstall(installJobId);
  }, [signal, followInstall]);

  async function choose(chosen: File | null) {
    setFile(chosen);
    setSha256(null);
    setHashed(null);
    setFileProblem(null);
    setUploadError(null);
    setUploadNotice(null);
    if (!chosen) return;
    const max = capabilities.data?.limits.upload_max_bytes;
    if (chosen.size === 0) {
      setFileProblem('update.file_empty');
      return;
    }
    if (max !== undefined && chosen.size > max) {
      setFileProblem('update.file_too_large');
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
          });
        } catch (error) {
          if (isApiFailure(error, 'busy')) {
            setUploadNotice('update.upload_busy_unknown');
            return;
          }
          throw error;
        }
        keys.current.delete(action);
        remember({ uploadId: current.id });
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
          capabilities.data?.limits.upload_chunk_bytes ?? DEFAULT_CHUNK_BYTES,
        );
        setUpload(current);
        await verify(current);
      }
    } catch (error) {
      if (error instanceof Stop || signal.aborted) return;
      setUploadError(error);
      if (upload || error instanceof UploadStopped) {
        const id = error instanceof UploadStopped ? error.upload.id : upload?.id;
        if (id) setUpload(await getUpload(id, signal).catch(() => null));
      }
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
      remember({ uploadId: null });
      setUpload(null);
      setAcknowledged(false);
    } catch (error) {
      if (isApiFailure(error, 'not_found')) {
        remember({ uploadId: null });
        setUpload(null);
      } else if (!(error instanceof Stop) && !signal.aborted) {
        setUploadError(error);
      }
    } finally {
      if (!signal.aborted) setBusy(null);
    }
  }

  async function install() {
    if (!upload || upload.state !== 'ready' || !acknowledged) return;
    setInstallError(null);
    setInstallNotice(null);
    setCancelRefused(false);
    setInstallJob(null);
    const action = `install:${upload.id}`;
    let jobId: string;
    try {
      jobId = (await startCoprocessorUpdate(session.csrf_token, keyFor(action), upload.id)).job_id;
    } catch (error) {
      if (!signal.aborted) setInstallError(error);
      return;
    }
    keys.current.delete(action);
    remember({ installJobId: jobId });
    setAcknowledged(false);
    await followInstall(jobId);
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
        setCancelRefused(true);
      } else if (!signal.aborted) {
        setInstallError(error);
      }
    }
  }

  const max = capabilities.data?.limits.upload_max_bytes;
  const uartUpdate = status.data?.uart_update;
  const canInstall =
    upload?.state === 'ready' && acknowledged && !installing && busy === null && uartUpdate?.available !== false;
  const resumable =
    upload !== null && file !== null && upload.state === 'receiving' && upload.size_bytes === file.size && upload.sha256 === sha256;

  return (
    <main>
      <h1>{t('update.title')}</h1>
      <Body polled={status} render={(s) => <StatusCard status={s} />} />

      <Card title="update.file">
        <p>{t('update.file_intro')}</p>
        <div className="field">
          <label htmlFor="firmware-file">{t('update.file_label')}</label>
          <input
            id="firmware-file"
            type="file"
            accept=".bin,application/octet-stream"
            disabled={busy !== null || installing}
            onChange={(e) => void choose(e.target.files?.[0] ?? null)}
          />
        </div>
        {fileProblem ? (
          <p className="field-error" role="alert">
            {t(fileProblem, { max: max ?? 0 })}
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
              ['update.sha256', <code key="sha" data-testid="file-sha256">{sha256}</code>],
            ]}
          />
        ) : null}
      </Card>

      <Card title="update.upload">
        {upload ? (
          <>
            <Facts
              rows={[
                ['update.upload_file', upload.filename],
                ['update.upload_state', <span key="state" data-testid="upload-state">{t(`update.upload_state.${upload.state}`)}</span>],
                ['update.size', t('update.bytes', { n: upload.size_bytes })],
              ]}
            />
            {upload.state === 'receiving' ? (
              <>
                <p className="muted" data-testid="upload-progress">
                  {t('update.uploading', { received: upload.received_bytes, total: upload.size_bytes })}
                </p>
                <progress className="upload-progress" value={upload.received_bytes} max={upload.size_bytes} />
              </>
            ) : null}
            {upload.state === 'failed' && upload.error ? (
              <p className="notice notice-error" role="alert" data-testid="verify-failed">
                {t('update.verify_failed', { reason: '' })}
                <DetailText detail={upload.error} />
              </p>
            ) : null}
            {upload.state === 'ready' && upload.image ? (
              <>
                <h3>{t('update.image')}</h3>
                <Facts
                  rows={[
                    ['update.image_format', t(`update.format.${upload.image.format}`)],
                    ['update.image_version', <code key="version" data-testid="image-version">{upload.image.version}</code>],
                    ['update.layout', upload.image.partition_layout_id ?? t('value.none')],
                    ['update.host_protocol', upload.image.host_protocol ?? t('value.none')],
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
          <p className="notice notice-warning" role="alert">
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
          <button type="button" disabled={!file || !sha256 || busy !== null || installing} onClick={() => void send()}>
            {t(resumable && upload && upload.received_bytes > 0 ? 'update.upload_resume' : 'update.upload_submit')}
          </button>
          {upload && upload.state === 'receiving' && upload.received_bytes === upload.size_bytes ? (
            <button type="button" className="button-secondary" disabled={busy !== null} onClick={() => void reverify()}>
              {t('update.verify_submit')}
            </button>
          ) : null}
          {upload ? (
            <button type="button" className="button-secondary" disabled={busy !== null || installing} onClick={() => void remove()}>
              {t('update.delete_submit')}
            </button>
          ) : null}
        </div>
      </Card>

      <Card title="update.install">
        <p className="notice notice-warning" role="note">
          {t('update.install_warning')}
        </p>
        {uartUpdate && !uartUpdate.available ? (
          <p className="muted" data-testid="install-unavailable">
            {t('update.install_unavailable', { reason: reasonText(uartUpdate.reason) })}
          </p>
        ) : null}
        {upload?.state !== 'ready' && !installing && !installJob ? <p className="muted">{t('update.install_needs_ready')}</p> : null}
        {upload?.state === 'ready' && !installing ? (
          <div className="field field-check">
            <label>
              <input type="checkbox" checked={acknowledged} onChange={(e) => setAcknowledged(e.target.checked)} />
              {t('update.install_acknowledge')}
            </label>
          </div>
        ) : null}
        <div className="actions">
          <button type="button" disabled={!canInstall} onClick={() => void install()}>
            {t('update.install_submit')}
          </button>
          {installing && installJob?.cancellable ? (
            <button type="button" className="button-secondary" onClick={() => void cancel()}>
              {t('update.cancel_submit')}
            </button>
          ) : null}
        </div>
        {installing || installJob ? <Phases job={installJob} /> : null}
        {connectionLost ? (
          <p role="status" className="notice">
            {t('update.connection_lost')}
          </p>
        ) : null}
        {cancelRefused ? (
          <p role="alert" className="notice notice-warning">
            {t('update.cancel_refused')}
          </p>
        ) : null}
        {installNotice ? (
          <p role="status" className="notice" data-testid="install-notice">
            {t(installNotice)}
          </p>
        ) : null}
        {installJob && !installing && (installJob.state === 'failed' || installJob.state === 'interrupted') && installJob.error ? (
          <p role="alert" className="notice notice-error" data-testid="install-failed">
            {t(installJob.state === 'failed' ? 'update.install_failed' : 'update.install_interrupted', { reason: '' })}
            <DetailText detail={installJob.error} />
          </p>
        ) : null}
        {isApiFailure(installError, 'ethernet_required') ? (
          <p role="alert" className="notice notice-error" data-testid="ethernet-required">
            {t('update.ethernet_required')}
          </p>
        ) : isApiFailure(installError, 'busy') ? (
          <p role="alert" className="notice notice-error">
            {t('update.busy')}
          </p>
        ) : installError ? (
          <ErrorNotice error={installError} />
        ) : null}
      </Card>
    </main>
  );
}

