import { type FormEvent, useState } from 'react';

import { login } from '../../api/auth';
import { ErrorNotice } from '../../components/ErrorNotice';
import { HttpWarning, PasswordField } from '../../components/ui';
import { t } from '../../i18n';
import { useAuth } from '../../state/auth';

export function LoginScreen({ notice }: { notice: 'session_ended' | 'password_changed' | null }) {
  const { signedIn } = useAuth();
  const [password, setPassword] = useState('');
  const [failure, setFailure] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);

  async function submit(event: FormEvent) {
    event.preventDefault();
    if (!password) return;
    setBusy(true);
    setFailure(null);
    try {
      signedIn(await login(password));
    } catch (error) {
      setFailure(error);
      setPassword('');
    } finally {
      setBusy(false);
    }
  }

  return (
    <main className="narrow">
      <h1>{t('login.title')}</h1>
      <HttpWarning />
      {notice === 'session_ended' ? <p className="notice">{t('login.session_ended')}</p> : null}
      {notice === 'password_changed' ? <p className="notice">{t('login.password_changed')}</p> : null}
      <form onSubmit={submit} noValidate>
        <PasswordField
          id="login-password"
          label="login.password_label"
          value={password}
          onChange={setPassword}
          autoComplete="current-password"
          autoFocus
        />
        {failure ? <ErrorNotice error={failure} /> : null}
        <button type="submit" disabled={busy || !password}>
          {t('login.submit')}
        </button>
      </form>
    </main>
  );
}
