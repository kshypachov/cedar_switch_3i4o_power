import { type FormEvent, useEffect, useRef, useState } from 'react';

import { changePassword, logout, newIdempotencyKey } from '../../api/auth';
import { isApiFailure } from '../../api/errors';
import { pollJob } from '../../api/jobs';
import type { Session } from '../../api/types';
import { describeField, ErrorNotice } from '../../components/ErrorNotice';
import { formatDuration } from '../../components/format';
import { Card, Facts, HttpWarning, PasswordField } from '../../components/ui';
import { t } from '../../i18n';
import { useAuth } from '../../state/auth';
import { newPasswordProblem } from './passwordPolicy';

export function AccessScreen({ session }: { session: Session }) {
  const { signedOut } = useAuth();
  const [current, setCurrent] = useState('');
  const [next, setNext] = useState('');
  const [confirmation, setConfirmation] = useState('');
  const [currentProblem, setCurrentProblem] = useState<string | null>(null);
  const [nextProblem, setNextProblem] = useState<string | null>(null);
  const [failure, setFailure] = useState<unknown>(null);
  const [running, setRunning] = useState(false);
  // One key per intended change: a retry of the same passwords reuses it, so a
  // change the device already accepted is answered with its job instead of a
  // second one; different passwords get a new key, never a 409 conflict.
  const attempt = useRef<{ key: string; current: string; next: string } | null>(null);
  const unmounted = useRef(new AbortController());

  useEffect(() => {
    const controller = unmounted.current;
    return () => controller.abort();
  }, []);

  async function submit(event: FormEvent) {
    event.preventDefault();
    const nextLocal = newPasswordProblem(next, confirmation);
    const currentLocal = current ? null : t('password.required');
    setNextProblem(nextLocal);
    setCurrentProblem(currentLocal);
    setFailure(null);
    if (nextLocal || currentLocal) return;

    if (!attempt.current || attempt.current.current !== current || attempt.current.next !== next) {
      attempt.current = { key: newIdempotencyKey(), current, next };
    }
    setRunning(true);
    try {
      const accepted = await changePassword(session.csrf_token, attempt.current.key, current, next);
      const outcome = await pollJob(accepted.job_id, { signal: unmounted.current.signal });
      if (outcome.kind === 'session_ended' || outcome.job.state === 'succeeded') {
        signedOut('password_changed');
        return;
      }
      setFailure(null);
      setCurrentProblem(t('access.change_failed'));
    } catch (error) {
      if (unmounted.current.signal.aborted) return;
      if (isApiFailure(error, 'session_expired') || isApiFailure(error, 'authentication_required')) {
        signedOut('session_ended');
        return;
      }
      if (isApiFailure(error, 'invalid_credentials')) {
        setCurrentProblem(t('error.invalid_credentials'));
      } else if (isApiFailure(error, 'validation_failed')) {
        const fields = error.fieldCodes();
        setCurrentProblem(fields.has('/current_password') ? describeField(fields.get('/current_password')!) : null);
        setNextProblem(fields.has('/new_password') ? describeField(fields.get('/new_password')!) : null);
        if (fields.size === 0) setFailure(error);
      } else {
        setFailure(error);
      }
    } finally {
      if (!unmounted.current.signal.aborted) setRunning(false);
    }
  }

  async function signOut() {
    try {
      await logout(session.csrf_token);
    } catch {
      // Signed out on this side either way; the cookie expires on its own.
    }
    signedOut(null);
  }

  return (
    <main>
      <h1>{t('access.title')}</h1>
      <Card title="access.session">
        <Facts
          rows={[
            ['access.idle_timeout', formatDuration(session.idle_timeout_seconds)],
            ['access.absolute_remaining', formatDuration(session.absolute_remaining_seconds)],
          ]}
        />
      </Card>
      <Card title="access.change_title">
        <p>{t('access.change_intro')}</p>
        <form onSubmit={submit} noValidate>
          <PasswordField
            id="current-password"
            label="access.current_password_label"
            value={current}
            onChange={setCurrent}
            autoComplete="current-password"
            error={currentProblem}
          />
          <PasswordField
            id="new-password"
            label="access.new_password_label"
            value={next}
            onChange={setNext}
            autoComplete="new-password"
          />
          <PasswordField
            id="new-password-confirmation"
            label="access.new_password_confirm_label"
            value={confirmation}
            onChange={setConfirmation}
            autoComplete="new-password"
            error={nextProblem}
          />
          {failure ? <ErrorNotice error={failure} /> : null}
          {running ? (
            <p role="status" className="muted">
              {t('access.change_running')}
            </p>
          ) : null}
          <button type="submit" disabled={running}>
            {t('access.change_submit')}
          </button>
        </form>
      </Card>
      <Card title="access.logout_title">
        <p>{t('access.logout_intro')}</p>
        <button type="button" className="button-secondary" onClick={signOut}>
          {t('nav.logout')}
        </button>
      </Card>
      <HttpWarning />
    </main>
  );
}
