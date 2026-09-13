import { api, newIdempotencyKey, unwrap } from './client';
import type { CommissioningWindow, Fabrics, JobAccepted, OnboardingCodes } from './types';

export const getCommissioningWindow = (signal?: AbortSignal): Promise<CommissioningWindow> =>
  unwrap(api.GET('/matter/commissioning', { signal }));

export const getOnboardingCodes = (signal?: AbortSignal): Promise<OnboardingCodes> =>
  unwrap(api.GET('/matter/onboarding-codes', { signal }));

export const listFabrics = (signal?: AbortSignal): Promise<Fabrics> =>
  unwrap(api.GET('/matter/fabrics', { signal }));

/** The key is kept by the caller for retries of the same request (see AccessScreen). */
export function openCommissioningWindow(
  csrfToken: string,
  idempotencyKey: string,
  timeoutSeconds: number,
): Promise<JobAccepted> {
  return unwrap(
    api.POST('/matter/commissioning', {
      params: { header: { 'X-CSRF-Token': csrfToken, 'Idempotency-Key': idempotencyKey } },
      body: { mode: 'basic', timeout_seconds: timeoutSeconds },
    }),
  );
}

export function closeCommissioningWindow(csrfToken: string, idempotencyKey: string): Promise<JobAccepted> {
  return unwrap(
    api.DELETE('/matter/commissioning', {
      params: { header: { 'X-CSRF-Token': csrfToken, 'Idempotency-Key': idempotencyKey } },
    }),
  );
}

export { newIdempotencyKey };
