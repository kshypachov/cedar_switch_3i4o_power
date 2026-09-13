import { describe, expect, it } from 'vitest';

import { pollJob } from './jobs';
import { fakeDevice, json, refusal } from '../test/server';
import { passwordJobRunning } from '../test/fixtures';

describe('pollJob', () => {
  it('follows a job to its terminal state, reporting each step', async () => {
    const states = ['queued', 'running', 'succeeded'] as const;
    let i = 0;
    fakeDevice({
      'GET /api/v1/jobs/job_00000001': () => json(200, { ...passwordJobRunning, state: states[Math.min(i++, 2)] }),
    });
    const seen: string[] = [];
    const outcome = await pollJob('job_00000001', { intervalMs: 1, onUpdate: (j) => seen.push(j.state) });
    expect(seen).toEqual(['queued', 'running', 'succeeded']);
    expect(outcome).toMatchObject({ kind: 'finished', job: { state: 'succeeded' } });
  });

  it("reads a password change's session revocation as the end", async () => {
    let i = 0;
    fakeDevice({
      'GET /api/v1/jobs/job_00000001': () => (i++ === 0 ? json(200, passwordJobRunning) : refusal(401, 'session_expired')),
    });
    expect(await pollJob('job_00000001', { intervalMs: 1 })).toEqual({ kind: 'session_ended' });
  });

  it('keeps asking through a transient transport failure', async () => {
    let i = 0;
    const device = fakeDevice({
      'GET /api/v1/jobs/job_00000001': () => json(200, { ...passwordJobRunning, state: 'succeeded' }),
    });
    device.fetch.mockImplementationOnce(async () => {
      i++;
      throw new TypeError('reset');
    });
    const outcome = await pollJob('job_00000001', { intervalMs: 1 });
    expect(i).toBe(1);
    expect(outcome.kind).toBe('finished');
  });

  it('gives up on a refusal that is not about the session', async () => {
    fakeDevice({ 'GET /api/v1/jobs/job_00000001': () => refusal(404, 'not_found') });
    await expect(pollJob('job_00000001', { intervalMs: 1 })).rejects.toMatchObject({ code: 'not_found' });
  });
});
