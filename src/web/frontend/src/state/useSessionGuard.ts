import { useEffect } from 'react';

import { isApiFailure } from '../api/errors';
import { useAuth } from './auth';
import type { Polled } from './usePolling';

/** A resource this firmware does not serve yet (its stage is later). */
export const notServed = (error: unknown) => isApiFailure(error, 'not_found');

/** Back to the login screen when any poll finds the session gone. */
export function useSessionGuard(...polls: Polled<unknown>[]) {
  const { signedOut } = useAuth();
  const expired = polls.some((p) => isApiFailure(p.error) && p.error.status === 401);
  useEffect(() => {
    if (expired) signedOut('session_ended');
  }, [expired, signedOut]);
}
