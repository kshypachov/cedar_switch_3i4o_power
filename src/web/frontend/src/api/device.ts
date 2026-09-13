import { api, unwrap } from './client';
import type {
  Capabilities,
  CoprocessorStatus,
  Job,
  MatterStatus,
  NetworkStatus,
  SystemStatus,
} from './types';

export const getSystemStatus = (signal?: AbortSignal): Promise<SystemStatus> =>
  unwrap(api.GET('/system/status', { signal }));

export const getCapabilities = (signal?: AbortSignal): Promise<Capabilities> =>
  unwrap(api.GET('/capabilities', { signal }));

export const getNetworkStatus = (signal?: AbortSignal): Promise<NetworkStatus> =>
  unwrap(api.GET('/network/status', { signal }));

export const getMatterStatus = (signal?: AbortSignal): Promise<MatterStatus> =>
  unwrap(api.GET('/matter/status', { signal }));

export const getCoprocessorStatus = (signal?: AbortSignal): Promise<CoprocessorStatus> =>
  unwrap(api.GET('/coprocessor/status', { signal }));

export const getJob = (jobId: string, signal?: AbortSignal): Promise<Job> =>
  unwrap(api.GET('/jobs/{job_id}', { params: { path: { job_id: jobId } }, signal }));
