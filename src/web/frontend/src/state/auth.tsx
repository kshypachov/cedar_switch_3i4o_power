import { createContext, type ReactNode, useCallback, useContext, useEffect, useState } from 'react';

import { getAuthState, getSession } from '../api/auth';
import { isApiFailure } from '../api/errors';
import type { AuthState, Session } from '../api/types';

/**
 * Where the browser stands with the device. The session token itself is an
 * HttpOnly cookie the page never sees; what the page keeps is the Session body
 * - the CSRF token and lifetimes - in memory only. Nothing goes to
 * localStorage (contract: "Password не хранить в localStorage").
 */
export type AuthView =
  | { kind: 'loading' }
  | { kind: 'unreachable'; error: unknown }
  | { kind: 'setup'; state: AuthState }
  | { kind: 'login'; notice: 'session_ended' | 'password_changed' | null }
  | { kind: 'locked' }
  | { kind: 'signed_in'; session: Session };

interface AuthApi {
  view: AuthView;
  refresh: () => Promise<void>;
  signedIn: (session: Session) => void;
  signedOut: (notice: 'session_ended' | 'password_changed' | null) => void;
}

const Context = createContext<AuthApi | null>(null);

export function AuthProvider({ children }: { children: ReactNode }) {
  const [view, setView] = useState<AuthView>({ kind: 'loading' });

  const refresh = useCallback(async () => {
    try {
      const state = await getAuthState();
      if (state.setup_allowed) {
        setView({ kind: 'setup', state });
        return;
      }
      if (state.setup_required) {
        // Someone else is in the middle of setup; show setup, which will
        // report the outcome when submitted.
        setView({ kind: 'setup', state });
        return;
      }
      try {
        setView({ kind: 'signed_in', session: await getSession() });
      } catch (error) {
        if (isApiFailure(error) && error.status === 401) {
          setView({ kind: 'login', notice: null });
        } else if (isApiFailure(error, 'service_not_ready')) {
          setView({ kind: 'locked' });
        } else {
          throw error;
        }
      }
    } catch (error) {
      setView({ kind: 'unreachable', error });
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const signedIn = useCallback((session: Session) => setView({ kind: 'signed_in', session }), []);
  const signedOut = useCallback(
    (notice: 'session_ended' | 'password_changed' | null) => setView({ kind: 'login', notice }),
    [],
  );

  return <Context.Provider value={{ view, refresh, signedIn, signedOut }}>{children}</Context.Provider>;
}

export function useAuth(): AuthApi {
  const value = useContext(Context);
  if (!value) throw new Error('useAuth outside AuthProvider');
  return value;
}
