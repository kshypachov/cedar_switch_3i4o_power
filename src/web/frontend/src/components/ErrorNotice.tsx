import { ApiFailure } from '../api/errors';
import { type MessageKey, t, tMaybe } from '../i18n';

/** The message the interface shows for a failed request. */
export function describeFailure(error: unknown): string {
  if (!(error instanceof ApiFailure)) return t('error.network');
  switch (error.kind) {
    case 'network':
      return t('error.network');
    case 'timeout':
      return t('error.timeout');
    case 'busy':
      return t('error.busy_transport');
    case 'protocol':
      return t('error.protocol', { status: error.status });
    case 'api': {
      const code = error.code ?? 'unknown';
      const text = tMaybe(`error.${code}`) ?? t('error.unknown', { code });
      return error.retryAfterSeconds
        ? `${text} ${t('error.retry_after', { seconds: error.retryAfterSeconds })}`
        : text;
    }
  }
}

/** The text for one field error code, e.g. from `fields` of a 422. */
export function describeField(code: string): string {
  return tMaybe(`field.${code}`) ?? t('field.invalid_format');
}

export function ErrorNotice({ error, onRetry }: { error: unknown; onRetry?: () => void }) {
  const requestId = error instanceof ApiFailure ? error.detail?.request_id : undefined;
  return (
    <div className="notice notice-error" role="alert">
      <p>{describeFailure(error)}</p>
      {requestId ? <p className="muted">{t('error.request_id', { id: requestId })}</p> : null}
      {onRetry ? (
        <button type="button" className="button-secondary" onClick={onRetry}>
          {t('error.retry' satisfies MessageKey)}
        </button>
      ) : null}
    </div>
  );
}
