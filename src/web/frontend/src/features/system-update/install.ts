import { ApiFailure } from '../../api/errors';
import { TERMINAL_JOB_STATES, type Job, type SystemStatus } from '../../api/types';

/**
 * Following an STM32 update across the device's own restart, apart from React
 * and the network so every path can be driven with a scripted device and a
 * fake clock.
 *
 * The job lives in the device's RAM (api-contract.md, "Обновление STM32"): it
 * reaches `rebooting`, the device restarts about two seconds later, and from
 * then on the job is gone - its outcome is `SystemFirmware.last_update`. So the
 * page follows the job only until it says `rebooting` (or ends), and then waits
 * for the device to come back with another `boot_id`.
 */

export const SYSTEM_PHASES = ['preparing', 'requesting', 'rebooting'] as const;
export type SystemPhase = (typeof SYSTEM_PHASES)[number];

export const JOB_POLL_MS = 500;
/** Unanswered job polls, after the swap was requested, that mean the device went down. */
export const DOWN_AFTER_FAILURES = 3;
export const STATUS_POLL_MS = 2_000;
/** MCUboot swaps a 1.4 MB image in ~34 s and the application starts at ~40 s; keep well past it. */
export const REBOOT_TIMEOUT_MS = 180_000;

const transient = (error: unknown): error is ApiFailure =>
  error instanceof ApiFailure && (error.kind === 'network' || error.kind === 'timeout' || error.kind === 'busy');

/** Anything but the contract's error body: the device is not answering as itself. */
const unanswered = (error: unknown): boolean =>
  transient(error) || (error instanceof ApiFailure && (error.kind === 'protocol' || error.status >= 500));

export interface JobDevice {
  getJob(jobId: string): Promise<Job>;
  sleep(ms: number): Promise<void>;
  onUpdate?(job: Job): void;
  onTransient?(): void;
}

export type FollowOutcome =
  /** The job ended before the restart: failed or cancelled (nothing was swapped). */
  | { kind: 'finished'; job: Job }
  /** The job reached `rebooting`, or the device went away after requesting the swap. */
  | { kind: 'rebooting'; job: Job | null }
  /** The job is unknown: the device already restarted (a reload found it gone). */
  | { kind: 'gone' }
  /** 401: the device restarted and its sessions went with it. */
  | { kind: 'session_ended' };

export async function followSystemInstall(device: JobDevice, jobId: string): Promise<FollowOutcome> {
  let last: Job | null = null;
  let failures = 0;
  for (;;) {
    try {
      const job = await device.getJob(jobId);
      failures = 0;
      last = job;
      device.onUpdate?.(job);
      if (TERMINAL_JOB_STATES.has(job.state)) {
        // A job that ended in rebooting is the device's way of saying the swap is on.
        return job.state === 'succeeded' ? { kind: 'rebooting', job } : { kind: 'finished', job };
      }
      if (job.phase === 'rebooting') return { kind: 'rebooting', job };
    } catch (error) {
      if (error instanceof ApiFailure && error.status === 401) return { kind: 'session_ended' };
      if (error instanceof ApiFailure && error.code === 'not_found') return { kind: 'gone' };
      if (!unanswered(error)) throw error;
      device.onTransient?.();
      failures++;
      // Before the request of the swap a silent device is just a slow network;
      // after it, the restart that was announced.
      if (last?.phase === 'requesting' && failures >= DOWN_AFTER_FAILURES) return { kind: 'rebooting', job: last };
    }
    await device.sleep(JOB_POLL_MS);
  }
}

export interface StatusDevice {
  getStatus(): Promise<SystemStatus>;
  sleep(ms: number): Promise<void>;
  now(): number;
  onWaiting?(elapsedMs: number): void;
}

export type RebootOutcome =
  | { kind: 'back'; status: SystemStatus }
  | { kind: 'session_ended' }
  | { kind: 'timeout'; elapsedMs: number };

/**
 * Wait for the device to answer with a boot_id other than @p previousBootId.
 * Silence is expected for tens of seconds and never ends the wait early; a 401
 * means the device is back but the session died with the restart.
 */
export async function waitForReboot(
  device: StatusDevice,
  previousBootId: string | null,
  timeoutMs = REBOOT_TIMEOUT_MS,
): Promise<RebootOutcome> {
  const start = device.now();
  for (;;) {
    const elapsed = device.now() - start;
    device.onWaiting?.(elapsed);
    try {
      const status = await device.getStatus();
      if (previousBootId === null || status.boot_id !== previousBootId) return { kind: 'back', status };
    } catch (error) {
      if (error instanceof ApiFailure && error.status === 401) return { kind: 'session_ended' };
      if (!unanswered(error)) throw error;
    }
    if (device.now() - start >= timeoutMs) return { kind: 'timeout', elapsedMs: device.now() - start };
    await device.sleep(STATUS_POLL_MS);
  }
}

/** Seconds left of a countdown the device reported @p reportedSeconds ago-at @p reportedAtMs. */
export function remainingSeconds(reportedSeconds: number, reportedAtMs: number, nowMs: number): number {
  return Math.max(0, reportedSeconds - Math.floor((nowMs - reportedAtMs) / 1000));
}
