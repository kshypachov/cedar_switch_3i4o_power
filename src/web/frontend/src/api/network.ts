import { api, newIdempotencyKey, unwrap } from './client';
import type {
  JobAccepted,
  NetworkConfigResponse,
  NetworkTransaction,
  NetworkTransactionRequest,
  ScanResults,
} from './types';

export const getNetworkConfig = (signal?: AbortSignal): Promise<NetworkConfigResponse> =>
  unwrap(api.GET('/network/config', { signal }));

export const getNetworkTransaction = (id: string, signal?: AbortSignal): Promise<NetworkTransaction> =>
  unwrap(api.GET('/network/transactions/{transaction_id}', { params: { path: { transaction_id: id } }, signal }));

export const getWiFiScan = (jobId: string, signal?: AbortSignal): Promise<ScanResults> =>
  unwrap(api.GET('/network/wifi/scans/{job_id}', { params: { path: { job_id: jobId } }, signal }));

// Every mutation takes the key from the caller, which keeps it for retries of
// the same action (see AccessScreen): a lost answer repeated under the same key
// returns the first result instead of a second transaction or job.
const headers = (csrfToken: string, idempotencyKey: string) => ({
  'X-CSRF-Token': csrfToken,
  'Idempotency-Key': idempotencyKey,
});

export function stageNetworkConfig(
  csrfToken: string,
  idempotencyKey: string,
  body: NetworkTransactionRequest,
): Promise<NetworkTransaction> {
  return unwrap(api.POST('/network/transactions', { params: { header: headers(csrfToken, idempotencyKey) }, body }));
}

export function applyNetworkTransaction(
  csrfToken: string,
  idempotencyKey: string,
  id: string,
  confirmationTimeoutSeconds: number,
): Promise<JobAccepted> {
  return unwrap(
    api.POST('/network/transactions/{transaction_id}/apply', {
      params: { path: { transaction_id: id }, header: headers(csrfToken, idempotencyKey) },
      body: { confirmation_timeout_seconds: confirmationTimeoutSeconds },
    }),
  );
}

/** Answered with the apply job, which the confirmation takes to its end. */
export function confirmNetworkTransaction(csrfToken: string, idempotencyKey: string, id: string): Promise<JobAccepted> {
  return unwrap(
    api.POST('/network/transactions/{transaction_id}/confirm', {
      params: { path: { transaction_id: id }, header: headers(csrfToken, idempotencyKey) },
      body: {},
    }),
  );
}

/** Discards a staged candidate (its own short job) or rolls back an applied one (the apply job). */
export function rollbackNetworkTransaction(csrfToken: string, idempotencyKey: string, id: string): Promise<JobAccepted> {
  return unwrap(
    api.DELETE('/network/transactions/{transaction_id}', {
      params: { path: { transaction_id: id }, header: headers(csrfToken, idempotencyKey) },
    }),
  );
}

export function scanWiFi(csrfToken: string, idempotencyKey: string): Promise<JobAccepted> {
  return unwrap(api.POST('/network/wifi/scans', { params: { header: headers(csrfToken, idempotencyKey) }, body: {} }));
}

export { newIdempotencyKey };
