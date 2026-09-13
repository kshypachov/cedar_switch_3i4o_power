import type { ReactNode } from 'react';

import { type MessageKey, t } from '../i18n';

export function Card({ title, children }: { title: MessageKey; children: ReactNode }) {
  return (
    <section className="card" aria-label={t(title)}>
      <h2>{t(title)}</h2>
      {children}
    </section>
  );
}

export function Facts({ rows }: { rows: [MessageKey, ReactNode][] }) {
  return (
    <dl className="facts">
      {rows.map(([label, value]) => (
        <div key={label} className="fact">
          <dt>{t(label)}</dt>
          <dd>{value}</dd>
        </div>
      ))}
    </dl>
  );
}

export function PasswordField(props: {
  id: string;
  label: MessageKey;
  value: string;
  onChange: (value: string) => void;
  error?: string | null;
  autoComplete: 'current-password' | 'new-password';
  autoFocus?: boolean;
}) {
  const errorId = `${props.id}-error`;
  return (
    <div className="field">
      <label htmlFor={props.id}>{t(props.label)}</label>
      <input
        id={props.id}
        type="password"
        value={props.value}
        onChange={(e) => props.onChange(e.target.value)}
        autoComplete={props.autoComplete}
        autoFocus={props.autoFocus}
        aria-invalid={props.error ? true : undefined}
        aria-describedby={props.error ? errorId : undefined}
      />
      {props.error ? (
        <p id={errorId} className="field-error">
          {props.error}
        </p>
      ) : null}
    </div>
  );
}

export function HttpWarning() {
  return (
    <p className="notice notice-warning" role="note">
      {t('app.http_warning')}
    </p>
  );
}

export function Loading() {
  return (
    <p className="muted" role="status">
      {t('app.loading')}
    </p>
  );
}
