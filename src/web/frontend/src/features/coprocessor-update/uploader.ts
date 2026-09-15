import { ApiFailure } from '../../api/errors';
import type { Job, JobAccepted, Upload } from '../../api/types';

/**
 * The chunk loop of an upload, apart from React and the network so every
 * recovery path can be tested with a scripted device.
 *
 * The contract's rules (api-contract.md, "HTTP flow"):
 * - `received_bytes` is the next acceptable offset and moves only when a chunk's
 *   job has written and flushed it, so the next chunk goes out only after the
 *   previous job succeeded;
 * - after any doubt - a lost answer, `offset_mismatch`, `busy` - the upload is
 *   read again and the loop continues from what the device says;
 * - a retry of the same chunk carries the same Idempotency-Key, so a chunk the
 *   device already accepted is answered with its job, not written twice; a
 *   different offset is a different request and gets a new key.
 */
export interface ChunkDevice {
  getUpload(uploadId: string): Promise<Upload>;
  putChunk(uploadId: string, offset: number, bytes: Uint8Array, idempotencyKey: string): Promise<JobAccepted>;
  /** The job at its end (the caller polls it). */
  waitJob(jobId: string): Promise<Job>;
  read(offset: number, length: number): Promise<Uint8Array>;
  newKey(): string;
  sleep(ms: number): Promise<void>;
  onProgress?(upload: Upload): void;
}

/** How many answers in a row may go missing before the upload gives up. */
export const TRANSIENT_LIMIT = 8;
export const RETRY_DELAY_MS = 1_000;

/** A chunk's job, or the upload itself, ended in a way no retry fixes. */
export class UploadStopped extends Error {
  readonly job: Job | null;
  readonly upload: Upload;

  constructor(upload: Upload, job: Job | null) {
    super(job ? `chunk job ${job.id} ${job.state}` : `upload ${upload.id} is ${upload.state}`);
    this.name = 'UploadStopped';
    this.upload = upload;
    this.job = job;
  }
}

const transient = (error: unknown): error is ApiFailure =>
  error instanceof ApiFailure && (error.kind === 'network' || error.kind === 'timeout' || error.kind === 'busy');

async function retrying<T>(device: ChunkDevice, action: () => Promise<T>): Promise<T> {
  for (let failures = 0; ; failures++) {
    try {
      return await action();
    } catch (error) {
      if (!transient(error) || failures + 1 >= TRANSIENT_LIMIT) throw error;
      await device.sleep(RETRY_DELAY_MS);
    }
  }
}

/**
 * Send what the device has not got yet, starting from @p upload, and return the
 * upload once every byte is received.
 */
export async function sendChunks(device: ChunkDevice, upload: Upload, chunkBytes: number): Promise<Upload> {
  let current = upload;
  let pending: { offset: number; key: string } | null = null;
  let failures = 0;

  while (current.received_bytes < current.size_bytes) {
    if (current.state !== 'receiving') throw new UploadStopped(current, null);

    if (current.active_job_id) {
      // A chunk this page (or a lost answer) already started: its end first.
      const job = await device.waitJob(current.active_job_id);
      if (job.state !== 'succeeded') throw new UploadStopped(current, job);
      current = await retrying(device, () => device.getUpload(current.id));
      device.onProgress?.(current);
      continue;
    }

    const offset = current.received_bytes;
    const length = Math.min(chunkBytes, current.size_bytes - offset);
    if (!pending || pending.offset !== offset) pending = { offset, key: device.newKey() };
    const bytes = await device.read(offset, length);

    let accepted: JobAccepted;
    try {
      accepted = await device.putChunk(current.id, offset, bytes, pending.key);
      failures = 0;
    } catch (error) {
      const code = error instanceof ApiFailure ? error.code : null;
      if (code === 'offset_mismatch' || code === 'busy') {
        // The device knows better where this upload stands: a chunk still
        // being written, or one that already landed.
        const before = current;
        current = await retrying(device, () => device.getUpload(current.id));
        if (code === 'busy' && !current.active_job_id && current.received_bytes === before.received_bytes) {
          await device.sleep(RETRY_DELAY_MS);
        }
        continue;
      }
      if (!transient(error) || ++failures >= TRANSIENT_LIMIT) throw error;
      await device.sleep(RETRY_DELAY_MS);
      current = await retrying(device, () => device.getUpload(current.id));
      continue;
    }

    const job = await device.waitJob(accepted.job_id);
    if (job.state !== 'succeeded') throw new UploadStopped(current, job);
    current = await retrying(device, () => device.getUpload(current.id));
    device.onProgress?.(current);
  }

  return current;
}
