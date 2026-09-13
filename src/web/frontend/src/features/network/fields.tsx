import type { ReactNode } from 'react';

import { type MessageKey, t } from '../../i18n';

interface Common {
  id: string;
  label: MessageKey;
  /** A problem with the value, shown next to the input and linked to it. */
  error?: string | null;
  hint?: ReactNode;
  disabled?: boolean;
}

function describedBy(props: Common): string | undefined {
  const ids = [props.hint ? `${props.id}-hint` : null, props.error ? `${props.id}-error` : null].filter(Boolean);
  return ids.length ? ids.join(' ') : undefined;
}

function Notes(props: Common) {
  return (
    <>
      {props.hint ? (
        <p id={`${props.id}-hint`} className="muted field-hint">
          {props.hint}
        </p>
      ) : null}
      {props.error ? (
        <p id={`${props.id}-error`} className="field-error">
          {props.error}
        </p>
      ) : null}
    </>
  );
}

export function TextField(
  props: Common & {
    value: string;
    onChange: (value: string) => void;
    type?: 'text' | 'password';
    inputMode?: 'text' | 'decimal' | 'numeric';
    autoComplete?: string;
  },
) {
  return (
    <div className="field">
      <label htmlFor={props.id}>{t(props.label)}</label>
      <input
        id={props.id}
        type={props.type ?? 'text'}
        value={props.value}
        onChange={(e) => props.onChange(e.target.value)}
        inputMode={props.inputMode}
        autoComplete={props.autoComplete ?? 'off'}
        spellCheck={false}
        disabled={props.disabled}
        aria-invalid={props.error ? true : undefined}
        aria-describedby={describedBy(props)}
      />
      <Notes {...props} />
    </div>
  );
}

export function SelectField<T extends string>(
  props: Common & { value: T; options: { value: T; text: string }[]; onChange: (value: T) => void },
) {
  return (
    <div className="field">
      <label htmlFor={props.id}>{t(props.label)}</label>
      <select
        id={props.id}
        value={props.value}
        onChange={(e) => props.onChange(e.target.value as T)}
        disabled={props.disabled}
        aria-invalid={props.error ? true : undefined}
        aria-describedby={describedBy(props)}
      >
        {props.options.map((o) => (
          <option key={o.value} value={o.value}>
            {o.text}
          </option>
        ))}
      </select>
      <Notes {...props} />
    </div>
  );
}

export function CheckboxField(props: Common & { checked: boolean; onChange: (checked: boolean) => void }) {
  return (
    <div className="field field-check">
      <label htmlFor={props.id}>
        <input
          id={props.id}
          type="checkbox"
          checked={props.checked}
          onChange={(e) => props.onChange(e.target.checked)}
          disabled={props.disabled}
          aria-invalid={props.error ? true : undefined}
          aria-describedby={describedBy(props)}
        />
        {t(props.label)}
      </label>
      <Notes {...props} />
    </div>
  );
}
