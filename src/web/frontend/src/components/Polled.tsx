import type { ReactNode } from 'react';

import { t } from '../i18n';
import type { Polled } from '../state/usePolling';
import { notServed } from '../state/useSessionGuard';
import { ErrorNotice } from './ErrorNotice';
import { Loading } from './ui';

/** The data, or why there is none: not served by this firmware, an error, loading. */
export function Body<T>({ polled, render }: { polled: Polled<T>; render: (data: T) => ReactNode }) {
  if (polled.data) return <>{render(polled.data)}</>;
  if (notServed(polled.error)) return <p className="muted">{t('overview.unavailable')}</p>;
  if (polled.error) return <ErrorNotice error={polled.error} />;
  return <Loading />;
}
