import { afterEach, describe, expect, it } from 'vitest';

import type { Job } from '../../api/types';
import { systemInstallRequesting } from '../../test/fixtures';
import { phaseRowsOf } from '../coprocessor-update/phases';
import { memoryAt, recall, remember } from '../coprocessor-update/remember';
import { SYSTEM_PHASES } from './install';
import { SYSTEM_STORAGE_KEY } from './SystemUpdateScreen';

afterEach(() => window.localStorage.clear());

describe('each update screen remembers on its own', () => {
  it('the STM32 entry and the ESP32 entry do not touch each other', () => {
    const system = memoryAt(SYSTEM_STORAGE_KEY);
    remember({ uploadId: 'upload_0001', installJobId: 'job_0001' });
    system.remember({ uploadId: 'upload_0002' });
    expect(recall()).toEqual({ uploadId: 'upload_0001', installJobId: 'job_0001' });
    expect(system.recall()).toEqual({ uploadId: 'upload_0002', installJobId: null });
    system.remember({ uploadId: null });
    expect(window.localStorage.getItem(SYSTEM_STORAGE_KEY)).toBeNull();
    expect(recall().uploadId).toBe('upload_0001');
    expect(SYSTEM_STORAGE_KEY).not.toBe('cedar.coprocessor-update');
  });
});

const rows = (job: Job | null) => phaseRowsOf(SYSTEM_PHASES, job).map((r) => `${r.phase}:${r.status}`);

describe('STM32 install phases', () => {
  it('are the contract’s three, in order', () => {
    expect([...SYSTEM_PHASES]).toEqual(['preparing', 'requesting', 'rebooting']);
  });

  it('place the job among them', () => {
    expect(rows(null)).toEqual(['preparing:pending', 'requesting:pending', 'rebooting:pending']);
    expect(rows({ ...systemInstallRequesting, phase: 'preparing' })).toEqual(['preparing:current', 'requesting:pending', 'rebooting:pending']);
    expect(rows(systemInstallRequesting)).toEqual(['preparing:done', 'requesting:current', 'rebooting:pending']);
    expect(rows({ ...systemInstallRequesting, phase: 'rebooting' })).toEqual(['preparing:done', 'requesting:done', 'rebooting:current']);
    expect(rows({ ...systemInstallRequesting, state: 'failed', phase: 'requesting' })).toEqual([
      'preparing:done',
      'requesting:stopped',
      'rebooting:pending',
    ]);
    expect(rows({ ...systemInstallRequesting, state: 'queued', phase: 'queued' })).toEqual([
      'preparing:pending',
      'requesting:pending',
      'rebooting:pending',
    ]);
    // A phase of another kind of job is not one of these.
    expect(rows({ ...systemInstallRequesting, phase: 'writing' })).toEqual(['preparing:pending', 'requesting:pending', 'rebooting:pending']);
  });
});
