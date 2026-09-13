import { type FormEvent, useState } from 'react';

import { setup } from '../../api/auth';
import { isApiFailure } from '../../api/errors';
import type { AuthState } from '../../api/types';
import { describeField, ErrorNotice } from '../../components/ErrorNotice';
import { HttpWarning, PasswordField } from '../../components/ui';
import { t } from '../../i18n';
import { useAuth } from '../../state/auth';
import { newPasswordProblem } from './passwordPolicy';

export function SetupScreen({ state }: { state: AuthState }) {
  const { signedIn, refresh } = useAuth();
  const [password, setPassword] = useState('');
  const [confirmation, setConfirmation] = useState('');
  const [problem, setProblem] = useState<string | null>(null);
  const [failure, setFailure] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);

  async function submit(event: FormEvent) {
    event.preventDefault();
    const local = newPasswordProblem(password, confirmation);
    setProblem(local);
    setFailure(null);
    if (local || !state.setup_token) return;
    setBusy(true);
    try {
      signedIn(await setup(state.setup_token, password));
    } catch (error) {
      const field = isApiFailure(error, 'validation_failed') ? error.fieldCodes().get('/password') : undefined;
      if (field) {
        setProblem(describeField(field));
      } else if (isApiFailure(error, 'setup_not_allowed')) {
        await refresh();
      } else {
        setFailure(error);
      }
    } finally {
      setBusy(false);
    }
  }

  return (
    <main className="narrow">
      <h1>{t('setup.title')}</h1>
      <HttpWarning />
      <p>{t('setup.intro')}</p>
      <div className="token" aria-label={t('setup.token_label')}>
        <span className="muted">{t('setup.token_label')}</span>
        <code data-testid="setup-token">{state.setup_token}</code>
        <p className="muted">{t('setup.token_hint')}</p>
      </div>
      <form onSubmit={submit} noValidate>
        <PasswordField
          id="setup-password"
          label="setup.password_label"
          value={password}
          onChange={setPassword}
          autoComplete="new-password"
          autoFocus
        />
        <PasswordField
          id="setup-confirmation"
          label="setup.password_confirm_label"
          value={confirmation}
          onChange={setConfirmation}
          autoComplete="new-password"
          error={problem}
        />
        {failure ? <ErrorNotice error={failure} /> : null}
        <button type="submit" disabled={busy || !state.setup_token}>
          {t('setup.submit')}
        </button>
      </form>
    </main>
  );
}
