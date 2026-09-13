import type { components } from './schema.gen';

type S = components['schemas'];

export type AuthState = S['AuthState'];
export type Session = S['Session'];
export type SystemStatus = S['SystemStatus'];
export type Capabilities = S['Capabilities'];
export type JobAccepted = S['JobAccepted'];
export type Job = S['Job'];
export type NetworkStatus = S['NetworkStatus'];
export type InterfaceStatus = S['InterfaceStatus'];
export type MatterStatus = S['MatterStatus'];
export type CommissioningWindow = S['CommissioningWindow'];
export type OnboardingCodes = S['OnboardingCodes'];
export type Fabric = S['Fabric'];
export type Fabrics = S['Fabrics'];
export type CoprocessorStatus = S['CoprocessorStatus'];

export const TERMINAL_JOB_STATES: ReadonlySet<Job['state']> = new Set([
  'succeeded',
  'failed',
  'cancelled',
  'interrupted',
]);
