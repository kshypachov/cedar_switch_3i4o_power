import { describe, expect, it } from 'vitest';

import { ApiFailure } from '../../api/errors';
import type { Job, JobAccepted, Upload } from '../../api/types';
import { type ChunkDevice, sendChunks, TRANSIENT_LIMIT, UploadStopped } from './uploader';

const CHUNK = 4;

function upload(over: Partial<Upload> = {}): Upload {
  return {
    id: 'upload_0001',
    target: 'esp32c6',
    filename: 'merged.bin',
    size_bytes: 10,
    received_bytes: 0,
    sha256: 'a'.repeat(64),
    state: 'receiving',
    active_job_id: null,
    image: null,
    error: null,
    ...over,
  };
}

const job = (id: string, state: Job['state'] = 'succeeded'): Job => ({
  id,
  boot_id: 'boot_1',
  kind: 'upload_chunk',
  state,
  phase: 'writing',
  progress: null,
  cancellable: false,
  created_uptime_ms: '1',
  updated_uptime_ms: '2',
  resource_url: '/api/v1/firmware/uploads/upload_0001',
  error: state === 'succeeded' ? null : { code: 'storage_full', message: 'full', request_id: 'req_1', retryable: false },
});

const accepted = (id: string): JobAccepted => ({ job_id: id, job_url: `/api/v1/jobs/${id}`, resource_url: null });
const api = (status: number, code: string) =>
  new ApiFailure('api', status, { code, message: code, request_id: 'req_1', retryable: false });

interface Put {
  offset: number;
  length: number;
  key: string;
}

/**
 * A device that stores bytes like the real one: a chunk's job completes on the
 * next waitJob, and only then does received_bytes move. @p script can replace
 * the answer to the n-th PUT.
 */
function scripted(start: Upload, script: Record<number, (put: Put) => JobAccepted | never> = {}) {
  let state = { ...start };
  const puts: Put[] = [];
  const jobs = new Map<string, { offset: number; length: number; outcome: Job['state'] }>();
  let keys = 0;
  let sleeps = 0;
  let nextJob = 1;
  const progress: number[] = [];
  const device: ChunkDevice = {
    async getUpload() {
      return { ...state };
    },
    async putChunk(_id, offset, bytes, key) {
      const put = { offset, length: bytes.length, key };
      puts.push(put);
      const scriptedAnswer = script[puts.length];
      if (scriptedAnswer) return scriptedAnswer(put);
      if (offset !== state.received_bytes) throw api(409, 'offset_mismatch');
      const id = `job_${nextJob++}`;
      jobs.set(id, { offset, length: bytes.length, outcome: 'succeeded' });
      state = { ...state, active_job_id: id };
      return accepted(id);
    },
    async waitJob(id) {
      const j = jobs.get(id);
      if (!j) return job(id);
      if (j.outcome === 'succeeded' && state.active_job_id === id) {
        state = { ...state, received_bytes: j.offset + j.length, active_job_id: null };
      }
      if (j.outcome !== 'succeeded') state = { ...state, active_job_id: null };
      return job(id, j.outcome);
    },
    async read(offset, length) {
      return new Uint8Array(length).fill(offset);
    },
    newKey: () => `key-${++keys}`,
    async sleep() {
      sleeps++;
    },
    onProgress: (u) => progress.push(u.received_bytes),
  };
  return {
    device,
    puts,
    jobs,
    progress,
    get sleeps() {
      return sleeps;
    },
    set: (change: Partial<Upload>) => {
      state = { ...state, ...change };
    },
    get state() {
      return state;
    },
  };
}

describe('the chunk loop', () => {
  it('sends every chunk in order, each after the previous job, each with its own key', async () => {
    const dev = scripted(upload());
    const done = await sendChunks(dev.device, upload(), CHUNK);
    expect(done.received_bytes).toBe(10);
    expect(dev.puts.map((p) => [p.offset, p.length])).toEqual([
      [0, 4],
      [4, 4],
      [8, 2],
    ]);
    expect(new Set(dev.puts.map((p) => p.key)).size).toBe(3);
    expect(dev.progress).toEqual([4, 8, 10]);
  });

  it('resumes from what the device received, not from zero', async () => {
    const start = upload({ received_bytes: 8 });
    const dev = scripted(start);
    await sendChunks(dev.device, start, CHUNK);
    expect(dev.puts.map((p) => p.offset)).toEqual([8]);
  });

  it('does nothing for an upload that already has every byte', async () => {
    const start = upload({ received_bytes: 10 });
    const dev = scripted(start);
    expect((await sendChunks(dev.device, start, CHUNK)).received_bytes).toBe(10);
    expect(dev.puts).toEqual([]);
  });

  it('waits for a chunk job the device is still running before sending the next', async () => {
    const start = upload({ received_bytes: 4, active_job_id: 'job_old' });
    const dev = scripted(start);
    dev.jobs.set('job_old', { offset: 4, length: 4, outcome: 'succeeded' });
    await sendChunks(dev.device, start, CHUNK);
    expect(dev.puts.map((p) => p.offset)).toEqual([8]);
  });

  it('stops when the chunk job it found running failed', async () => {
    const start = upload({ received_bytes: 4, active_job_id: 'job_old' });
    const dev = scripted(start);
    dev.jobs.set('job_old', { offset: 4, length: 4, outcome: 'failed' });
    const error = await sendChunks(dev.device, start, CHUNK).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(UploadStopped);
    expect((error as UploadStopped).job?.id).toBe('job_old');
    expect(dev.puts).toEqual([]);
  });

  it("repeats a chunk the server's bare 409 turned away, under the same key", async () => {
    const dev = scripted(upload(), {
      1: () => {
        throw new ApiFailure('busy', 409, null);
      },
    });
    await sendChunks(dev.device, upload(), CHUNK);
    expect(dev.puts.map((p) => p.offset)).toEqual([0, 0, 4, 8]);
    expect(dev.puts[0]!.key).toBe(dev.puts[1]!.key);
  });

  it('repeats a chunk whose answer was lost under the same key', async () => {
    const dev = scripted(upload(), {
      2: () => {
        throw new ApiFailure('network', 0, null);
      },
    });
    await sendChunks(dev.device, upload(), CHUNK);
    expect(dev.puts.map((p) => p.offset)).toEqual([0, 4, 4, 8]);
    expect(dev.puts[1]!.key).toBe(dev.puts[2]!.key);
    expect(dev.puts[2]!.key).not.toBe(dev.puts[3]!.key);
    expect(dev.sleeps).toBeGreaterThan(0);
  });

  it('continues from the device offset after offset_mismatch', async () => {
    // A lost answer to a chunk that did land: the device is already further on.
    const dev = scripted(upload(), {
      1: () => {
        dev.set({ received_bytes: 4 });
        throw api(409, 'offset_mismatch');
      },
    });
    await sendChunks(dev.device, upload(), CHUNK);
    expect(dev.puts.map((p) => p.offset)).toEqual([0, 4, 8]);
  });

  it('on busy follows the job the device is running, then goes on', async () => {
    const dev = scripted(upload(), {
      1: () => {
        dev.jobs.set('job_other', { offset: 0, length: 4, outcome: 'succeeded' });
        dev.set({ active_job_id: 'job_other' });
        throw api(409, 'busy');
      },
    });
    await sendChunks(dev.device, upload(), CHUNK);
    expect(dev.puts.map((p) => p.offset)).toEqual([0, 4, 8]);
  });

  it('on busy with nothing running waits before trying again', async () => {
    let refused = false;
    const dev = scripted(upload(), {
      1: () => {
        refused = true;
        throw api(409, 'busy');
      },
    });
    await sendChunks(dev.device, upload(), CHUNK);
    expect(refused).toBe(true);
    expect(dev.sleeps).toBe(1);
    expect(dev.puts[0]!.key).toBe(dev.puts[1]!.key);
  });

  it('stops when a chunk job fails, with the job', async () => {
    const dev = scripted(upload());
    const original = dev.device.putChunk;
    dev.device.putChunk = async (...args) => {
      const a = await original(...args);
      dev.jobs.get(a.job_id)!.outcome = 'failed';
      return a;
    };
    const error = await sendChunks(dev.device, upload(), CHUNK).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(UploadStopped);
    expect((error as UploadStopped).job?.error?.code).toBe('storage_full');
    expect(dev.puts).toHaveLength(1);
  });

  it('stops for an upload that is no longer receiving', async () => {
    const start = upload({ state: 'failed' });
    const dev = scripted(start);
    const error = await sendChunks(dev.device, start, CHUNK).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(UploadStopped);
    expect(dev.puts).toEqual([]);
  });

  it('passes a refusal that no retry fixes straight on', async () => {
    const dev = scripted(upload(), {
      1: () => {
        throw api(413, 'payload_too_large');
      },
    });
    const error = (await sendChunks(dev.device, upload(), CHUNK).catch((e: unknown) => e)) as ApiFailure;
    expect(error.code).toBe('payload_too_large');
    expect(dev.puts).toHaveLength(1);
  });

  it('gives up after a bounded number of lost answers in a row', async () => {
    const script: Record<number, () => never> = {};
    for (let i = 1; i <= TRANSIENT_LIMIT + 2; i++) {
      script[i] = () => {
        throw new ApiFailure('timeout', 0, null);
      };
    }
    const dev = scripted(upload(), script);
    const error = (await sendChunks(dev.device, upload(), CHUNK).catch((e: unknown) => e)) as ApiFailure;
    expect(error.kind).toBe('timeout');
    expect(dev.puts).toHaveLength(TRANSIENT_LIMIT);
  });

  it('counts lost answers in a row, not in total', async () => {
    // Three losses on every chunk: nine in total, never TRANSIENT_LIMIT in a row.
    const script: Record<number, () => never> = {};
    for (const n of [1, 2, 3, 5, 6, 7, 9, 10, 11]) {
      script[n] = () => {
        throw new ApiFailure('network', 0, null);
      };
    }
    const dev = scripted(upload(), script);
    expect((await sendChunks(dev.device, upload(), CHUNK)).received_bytes).toBe(10);
  });
});
