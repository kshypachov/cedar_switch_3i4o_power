import { describe, expect, it } from 'vitest';

import type { Job } from '../../api/types';
import { INSTALL_PHASES, phaseRows, reachedDestructive } from './phases';

const install = (over: Partial<Job>): Job => ({
  id: 'job_0100',
  boot_id: 'boot_1',
  kind: 'coprocessor_update',
  state: 'running',
  phase: 'preflight',
  progress: null,
  cancellable: true,
  created_uptime_ms: '1',
  updated_uptime_ms: '2',
  resource_url: '/api/v1/coprocessor/status',
  error: null,
  ...over,
});

const statuses = (job: Job | null) => phaseRows(job).map((r) => r.status);

describe('install phases', () => {
  it('lists the UART path in order, without activating and confirming', () => {
    expect([...INSTALL_PHASES]).toEqual([
      'preflight',
      'entering_bootloader',
      'begin',
      'writing',
      'verifying',
      'reconnecting',
      'health_check',
      'complete',
    ]);
  });

  it('is all ahead before the job runs', () => {
    expect(statuses(null)).toEqual(Array(8).fill('pending'));
    expect(statuses(install({ state: 'queued', phase: 'queued' }))).toEqual(Array(8).fill('pending'));
    // A queued job may already name a phase; nothing has run yet.
    expect(statuses(install({ state: 'queued', phase: 'writing' }))).toEqual(Array(8).fill('pending'));
  });

  it('marks what is behind, where it is and what is ahead, with progress only where it is', () => {
    const rows = phaseRows(install({ phase: 'writing', progress: { completed: 16384, total: 1468176, unit: 'bytes' } }));
    expect(rows.map((r) => r.status)).toEqual(['done', 'done', 'done', 'current', 'pending', 'pending', 'pending', 'pending']);
    expect(rows[3]!.progress).toEqual({ completed: 16384, total: 1468176, unit: 'bytes' });
    expect(rows.filter((r) => r.progress !== null)).toHaveLength(1);
  });

  it('is all done once it succeeded', () => {
    expect(statuses(install({ state: 'succeeded', phase: 'complete' }))).toEqual(Array(8).fill('done'));
  });

  it.each(['failed', 'interrupted', 'cancelled'] as const)('stops at the phase a %s job ended in', (state) => {
    expect(statuses(install({ state, phase: 'verifying' }))).toEqual([
      'done',
      'done',
      'done',
      'done',
      'stopped',
      'pending',
      'pending',
      'pending',
    ]);
  });

  it('does not guess for a phase it does not know', () => {
    expect(statuses(install({ phase: 'activating' }))).toEqual(Array(8).fill('pending'));
  });

  it('knows from which phase the chip is being erased', () => {
    expect(reachedDestructive(null)).toBe(false);
    expect(reachedDestructive(install({ phase: 'entering_bootloader' }))).toBe(false);
    expect(reachedDestructive(install({ phase: 'begin' }))).toBe(true);
    expect(reachedDestructive(install({ phase: 'health_check', state: 'failed' }))).toBe(true);
    expect(reachedDestructive(install({ phase: 'queued' }))).toBe(false);
  });
});
