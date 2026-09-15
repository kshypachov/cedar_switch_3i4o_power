import { getJob } from './device';
import { ApiFailure } from './errors';
import { TERMINAL_JOB_STATES, type Job } from './types';

/** The contract's poll interval for an active job: 500-1000 ms. */
export const JOB_POLL_MS = 750;

export type JobOutcome =
  | { kind: 'finished'; job: Job }
  /**
   * The session ended while polling. For a password change that is the
   * expected success signal: the job revokes every session, the poll that
   * follows is refused, and the job itself can no longer be read.
   */
  | { kind: 'session_ended' };

function wait(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(resolve, ms);
    signal?.addEventListener(
      'abort',
      () => {
        clearTimeout(timer);
        reject(signal.reason);
      },
      { once: true },
    );
  });
}

/** Poll one job until it is terminal, one request at a time. */
export async function pollJob(
  jobId: string,
  options: {
    signal?: AbortSignal;
    onUpdate?: (job: Job) => void;
    /** A poll that got no answer (busy, network, timeout); polling goes on. */
    onTransient?: (error: ApiFailure) => void;
    intervalMs?: number;
  } = {},
): Promise<JobOutcome> {
  const interval = options.intervalMs ?? JOB_POLL_MS;
  for (;;) {
    try {
      const job = await getJob(jobId, options.signal);
      options.onUpdate?.(job);
      if (TERMINAL_JOB_STATES.has(job.state)) return { kind: 'finished', job };
    } catch (error) {
      if (error instanceof ApiFailure && error.status === 401) return { kind: 'session_ended' };
      if (!(error instanceof ApiFailure) || error.kind === 'api') throw error;
      // busy, network, timeout: the job keeps running; keep asking.
      options.onTransient?.(error);
    }
    await wait(interval, options.signal);
  }
}
