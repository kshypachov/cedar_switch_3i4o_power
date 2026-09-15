import type { Job, Progress } from '../../api/types';

/**
 * The phases of a UART install, in the order the device runs them
 * (api-contract.md: `activating` and `confirming` are not used on this path).
 */
export const INSTALL_PHASES = [
  'preflight',
  'entering_bootloader',
  'begin',
  'writing',
  'verifying',
  'reconnecting',
  'health_check',
  'complete',
] as const;

export type InstallPhase = (typeof INSTALL_PHASES)[number];
export type PhaseStatus = 'done' | 'current' | 'pending' | 'stopped';

export interface PhaseRow {
  phase: InstallPhase;
  status: PhaseStatus;
  /** Progress of the current phase only: the contract resets it at every phase. */
  progress: Progress | null;
}

const isPhase = (name: string): name is InstallPhase => (INSTALL_PHASES as readonly string[]).includes(name);

/** One row per phase: what is behind the job, where it is, what is ahead. */
export function phaseRows(job: Job | null): PhaseRow[] {
  const rows = (status: (i: number) => PhaseStatus, at = -1): PhaseRow[] =>
    INSTALL_PHASES.map((phase, i) => ({ phase, status: status(i), progress: i === at ? (job?.progress ?? null) : null }));

  if (!job || job.state === 'queued') return rows(() => 'pending');
  if (job.state === 'succeeded') return rows(() => 'done');

  const at = isPhase(job.phase) ? INSTALL_PHASES.indexOf(job.phase) : -1;
  if (at < 0) return rows(() => 'pending');

  const here: PhaseStatus = job.state === 'running' || job.state === 'waiting_confirmation' ? 'current' : 'stopped';
  return rows((i) => (i < at ? 'done' : i === at ? here : 'pending'), at);
}

/** Whether the job reached the phase that erases the chip: from there on it cannot be undone. */
export function reachedDestructive(job: Job | null): boolean {
  if (!job || !isPhase(job.phase)) return false;
  return INSTALL_PHASES.indexOf(job.phase) >= INSTALL_PHASES.indexOf('begin');
}
