import { api, newIdempotencyKey, unwrap } from './client';
import type { AuthState, JobAccepted, Session } from './types';

export function getAuthState(signal?: AbortSignal): Promise<AuthState> {
  return unwrap(api.GET('/auth/state', { signal }));
}

export function setup(token: string, password: string): Promise<Session> {
  return unwrap(
    api.POST('/auth/setup', {
      params: { header: { 'X-Setup-Token': token } },
      body: { password },
    }),
  );
}

export function login(password: string): Promise<Session> {
  return unwrap(api.POST('/auth/session', { body: { password } }));
}

export function getSession(signal?: AbortSignal): Promise<Session> {
  return unwrap(api.GET('/auth/session', { signal }));
}

export async function logout(csrfToken: string): Promise<void> {
  await unwrap(api.DELETE('/auth/session', { params: { header: { 'X-CSRF-Token': csrfToken } } }));
}

/**
 * The key is made by the caller and kept for retries of the same change: the
 * contract answers a repeat under one key with the original job.
 */
export function changePassword(
  csrfToken: string,
  idempotencyKey: string,
  currentPassword: string,
  newPassword: string,
): Promise<JobAccepted> {
  return unwrap(
    api.PUT('/auth/password', {
      params: { header: { 'X-CSRF-Token': csrfToken, 'Idempotency-Key': idempotencyKey } },
      body: { current_password: currentPassword, new_password: newPassword },
    }),
  );
}

export { newIdempotencyKey };
