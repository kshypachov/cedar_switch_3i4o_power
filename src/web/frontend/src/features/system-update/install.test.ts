import { describe, expect, it } from 'vitest';

import { ApiFailure } from '../../api/errors';
import type { Job, SystemStatus } from '../../api/types';
import { systemInstallRequesting, systemStatus } from '../../test/fixtures';
import {
  DOWN_AFTER_FAILURES,
  followSystemInstall,
  JOB_POLL_MS,
  REBOOT_TIMEOUT_MS,
  remainingSeconds,
  STATUS_POLL_MS,
  waitForReboot,
} from './install';

const network = () => new ApiFailure('network', 0, null);
const refused = (status: number, code: string) =>
  new ApiFailure('api', status, { code, message: code, request_id: 'req_1', retryable: false });

const job = (over: Partial<Job>): Job => ({ ...systemInstallRequesting, ...over });

/** A job device answering from a script; each entry is a job or an error to throw. */
function jobDevice(script: (Job | Error)[]) {
  const slept: number[] = [];
  const updates: string[] = [];
  let transients = 0;
  let calls = 0;
  return {
    slept,
    updates,
    get transients() {
      return transients;
    },
    get calls() {
      return calls;
    },
    device: {
      getJob: async () => {
        const next = script[Math.min(calls++, script.length - 1)]!;
        if (next instanceof Error) throw next;
        return next;
      },
      sleep: async (ms: number) => {
        slept.push(ms);
      },
      onUpdate: (j: Job) => updates.push(`${j.state}:${j.phase}`),
      onTransient: () => {
        transients++;
      },
    },
  };
}

describe('following the install job up to the restart', () => {
  it('stops at rebooting, with that job', async () => {
    const d = jobDevice([job({ phase: 'preparing', cancellable: true }), job({ phase: 'requesting' }), job({ phase: 'rebooting' })]);
    const outcome = await followSystemInstall(d.device, 'job_00000201');
    expect(outcome).toEqual({ kind: 'rebooting', job: job({ phase: 'rebooting' }) });
    expect(d.updates).toEqual(['running:preparing', 'running:requesting', 'running:rebooting']);
    expect(d.slept).toEqual([JOB_POLL_MS, JOB_POLL_MS]);
  });

  it('a job that failed or was cancelled ends the follow without a restart', async () => {
    const failed = job({ state: 'failed', phase: 'preparing' });
    expect(await followSystemInstall(jobDevice([failed]).device, 'j')).toEqual({ kind: 'finished', job: failed });
    const cancelled = job({ state: 'cancelled', phase: 'preparing' });
    expect(await followSystemInstall(jobDevice([cancelled]).device, 'j')).toEqual({ kind: 'finished', job: cancelled });
  });

  it('a job that succeeded is the restart announced, not an ending', async () => {
    const done = job({ state: 'succeeded', phase: 'rebooting' });
    expect(await followSystemInstall(jobDevice([done]).device, 'j')).toEqual({ kind: 'rebooting', job: done });
  });

  it('silence after the swap was requested is the restart', async () => {
    const d = jobDevice([job({ phase: 'requesting' }), ...Array.from({ length: DOWN_AFTER_FAILURES }, network)]);
    const outcome = await followSystemInstall(d.device, 'j');
    expect(outcome).toEqual({ kind: 'rebooting', job: job({ phase: 'requesting' }) });
    expect(d.transients).toBe(DOWN_AFTER_FAILURES);
    expect(d.calls).toBe(1 + DOWN_AFTER_FAILURES);
  });

  it('one silence fewer than the limit keeps asking', async () => {
    const d = jobDevice([
      job({ phase: 'requesting' }),
      ...Array.from({ length: DOWN_AFTER_FAILURES - 1 }, network),
      job({ phase: 'rebooting' }),
    ]);
    expect((await followSystemInstall(d.device, 'j')).kind).toBe('rebooting');
    expect(d.calls).toBe(1 + DOWN_AFTER_FAILURES);
  });

  it('an answer between silences starts the count again', async () => {
    const d = jobDevice([
      job({ phase: 'requesting' }),
      network(),
      network(),
      job({ phase: 'requesting' }),
      network(),
      network(),
      job({ phase: 'rebooting' }),
    ]);
    const outcome = await followSystemInstall(d.device, 'j');
    expect(outcome).toEqual({ kind: 'rebooting', job: job({ phase: 'rebooting' }) });
    expect(d.calls).toBe(7);
  });

  it('silence before the request is only a slow network', async () => {
    const d = jobDevice([job({ phase: 'preparing' }), network(), network(), network(), network(), job({ state: 'failed', phase: 'preparing' })]);
    expect((await followSystemInstall(d.device, 'j')).kind).toBe('finished');
    expect(d.transients).toBe(4);
  });

  it('a busy or timed-out poll counts as silence, a 5xx too', async () => {
    const d = jobDevice([
      job({ phase: 'requesting' }),
      new ApiFailure('busy', 409, null),
      new ApiFailure('timeout', 0, null),
      refused(503, 'service_not_ready'),
    ]);
    expect((await followSystemInstall(d.device, 'j')).kind).toBe('rebooting');
  });

  it('404 means the device already restarted; 401 that the session went with it', async () => {
    expect(await followSystemInstall(jobDevice([refused(404, 'not_found')]).device, 'j')).toEqual({ kind: 'gone' });
    expect(await followSystemInstall(jobDevice([new ApiFailure('api', 401, null)]).device, 'j')).toEqual({ kind: 'session_ended' });
  });

  it('any other refusal is the caller’s to show', async () => {
    await expect(followSystemInstall(jobDevice([refused(400, 'invalid_query')]).device, 'j')).rejects.toThrow('invalid_query');
  });
});

/** A status device on a fake clock: every poll and every sleep advance it. */
function statusDevice(script: (SystemStatus | Error)[], pollCostMs = 0) {
  let now = 1_000;
  let calls = 0;
  const waited: number[] = [];
  return {
    waited,
    get calls() {
      return calls;
    },
    device: {
      now: () => now,
      sleep: async (ms: number) => {
        now += ms;
      },
      getStatus: async () => {
        now += pollCostMs;
        const next = script[Math.min(calls++, script.length - 1)]!;
        if (next instanceof Error) throw next;
        return next;
      },
      onWaiting: (elapsed: number) => waited.push(elapsed),
    },
  };
}

const boot = (bootId: string): SystemStatus => ({ ...systemStatus, boot_id: bootId });

describe('waiting for the device to come back', () => {
  it('returns once the boot id changes, through the silence of the swap', async () => {
    const d = statusDevice([boot('boot_a'), network(), network(), network(), boot('boot_b')]);
    const outcome = await waitForReboot(d.device, 'boot_a');
    expect(outcome).toEqual({ kind: 'back', status: boot('boot_b') });
    expect(d.calls).toBe(5);
    expect(d.waited).toEqual([0, 2000, 4000, 6000, 8000]);
  });

  it('the same boot id is not a restart', async () => {
    const d = statusDevice([boot('boot_a'), boot('boot_a'), boot('boot_b')]);
    expect((await waitForReboot(d.device, 'boot_a')).kind).toBe('back');
    expect(d.calls).toBe(3);
  });

  it('without a known boot the first answer is taken', async () => {
    const d = statusDevice([boot('boot_a')]);
    expect(await waitForReboot(d.device, null)).toEqual({ kind: 'back', status: boot('boot_a') });
  });

  it('401: back, but the session died with the restart', async () => {
    const d = statusDevice([network(), new ApiFailure('api', 401, null)]);
    expect(await waitForReboot(d.device, 'boot_a')).toEqual({ kind: 'session_ended' });
  });

  it('keeps waiting at least 90 seconds, and gives up at the timeout', async () => {
    const d = statusDevice([network()]);
    const outcome = await waitForReboot(d.device, 'boot_a');
    expect(outcome).toEqual({ kind: 'timeout', elapsedMs: REBOOT_TIMEOUT_MS });
    expect(REBOOT_TIMEOUT_MS).toBeGreaterThanOrEqual(90_000);
    expect(d.calls).toBe(REBOOT_TIMEOUT_MS / STATUS_POLL_MS + 1);
  });

  it('a slow poll counts against the timeout', async () => {
    const d = statusDevice([network()], 8_000);
    const outcome = await waitForReboot(d.device, 'boot_a', 20_000);
    expect(outcome.kind).toBe('timeout');
    expect(d.calls).toBe(3);
  });

  it('an answer on the last poll still wins over the timeout', async () => {
    const script = [...Array.from({ length: 10 }, network), boot('boot_b')];
    const d = statusDevice(script);
    expect((await waitForReboot(d.device, 'boot_a', 20_000)).kind).toBe('back');
  });

  it('any other refusal is thrown', async () => {
    const d = statusDevice([refused(403, 'csrf_failed')]);
    await expect(waitForReboot(d.device, 'boot_a')).rejects.toThrow('csrf_failed');
  });
});

describe('the confirmation countdown between polls', () => {
  it.each([
    [1140, 0, 1140],
    [1140, 999, 1140],
    [1140, 1000, 1139],
    [1140, 61_500, 1079],
    [5, 5_000, 0],
    [5, 60_000, 0],
  ])('%i s reported, %i ms later: %i s', (reported, later, expected) => {
    expect(remainingSeconds(reported, 10_000, 10_000 + later)).toBe(expected);
  });
});
