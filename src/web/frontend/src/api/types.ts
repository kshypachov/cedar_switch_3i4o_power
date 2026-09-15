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
export type Address = S['Address'];
export type IPv4Config = S['IPv4Config'];
export type CredentialChange = S['CredentialChange'];
export type WiFiConfigOutput = S['WiFiConfigOutput'];
export type NetworkConfigInput = S['NetworkConfigInput'];
export type NetworkConfigOutput = S['NetworkConfigOutput'];
export type NetworkConfigResponse = S['NetworkConfigResponse'];
export type NetworkTransactionRequest = S['NetworkTransactionRequest'];
export type NetworkTransaction = S['NetworkTransaction'];
export type ScanResults = S['ScanResults'];
export type AccessPoint = S['AccessPoint'];
export type LogSource = S['LogSource'];
export type LogSources = S['LogSources'];
export type LogRecord = S['LogRecord'];
export type LogPage = S['LogPage'];
export type Upload = S['Upload'];
export type UploadRequest = S['UploadRequest'];
export type FirmwareImage = S['FirmwareImage'];
export type UpdateSummary = S['UpdateSummary'];
export type FeatureAvailability = S['FeatureAvailability'];
export type Progress = S['Progress'];

export const TERMINAL_JOB_STATES: ReadonlySet<Job['state']> = new Set([
  'succeeded',
  'failed',
  'cancelled',
  'interrupted',
]);
