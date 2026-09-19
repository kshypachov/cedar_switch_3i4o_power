import { useCallback, useEffect, useRef, useState } from 'react';

import { isApiFailure } from '../../api/errors';
import { clearCoredump, coredumpUrl, getCoredump } from '../../api/system';
import type { CoredumpStatus, Session } from '../../api/types';
import { ErrorNotice } from '../../components/ErrorNotice';
import { Card, Facts, Loading } from '../../components/ui';
import { t, tMaybe } from '../../i18n';
import { useAuth } from '../../state/auth';

/** Size as the page shows it: KiB, one decimal under 10 KiB. */
function kib(bytes: number): string {
  const value = bytes / 1024;
  return value < 10 ? value.toFixed(1) : String(Math.round(value));
}

/**
 * The Zephyr coredump the device stored before the reset that followed a crash
 * (GET /system/coredump). Read once when the page opens and again after a
 * clear: a new dump only appears after a restart, which ends the session.
 */
export function CoredumpCard({ session }: { session: Session }) {
  const { signedOut } = useAuth();
  const [status, setStatus] = useState<CoredumpStatus | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [clearing, setClearing] = useState(false);
  const alive = useRef(new AbortController());

  const load = useCallback(async () => {
    setError(null);
    try {
      setStatus(await getCoredump(alive.current.signal));
    } catch (e) {
      if (!alive.current.signal.aborted) setError(e);
    }
  }, []);

  useEffect(() => {
    const controller = alive.current;
    void load();
    return () => controller.abort();
  }, [load]);

  async function clear() {
    setClearing(true);
    setError(null);
    try {
      await clearCoredump(session.csrf_token);
      await load();
    } catch (e) {
      if (alive.current.signal.aborted) return;
      if (isApiFailure(e, 'session_expired') || isApiFailure(e, 'authentication_required')) {
        signedOut('session_ended');
        return;
      }
      setError(e);
    } finally {
      setClearing(false);
    }
  }

  if (isApiFailure(error, 'capability_unavailable')) {
    return (
      <Card title="coredump.title">
        <p className="muted">{t('coredump.unavailable')}</p>
      </Card>
    );
  }

  const dump = status?.coredump ?? null;
  return (
    <Card title="coredump.title">
      <p>{t('coredump.intro')}</p>
      {error ? <ErrorNotice error={error} onRetry={() => void load()} /> : null}
      {status === null && !error ? (
        <Loading />
      ) : status !== null && dump === null ? (
        <p className="muted" data-testid="coredump-none">
          {t('coredump.none')}
        </p>
      ) : dump !== null ? (
        <>
          <Facts
            rows={[
              ['coredump.size', t('coredump.size_value', { kib: kib(dump.size_bytes), bytes: dump.size_bytes })],
              [
                'coredump.reason',
                dump.reason === null
                  ? t('value.unknown')
                  : `${tMaybe(`coredump.reason.${dump.reason}`) ?? dump.reason} (${dump.reason_code})`,
              ],
            ]}
          />
          <div className="actions">
            <a className="button-link" href={coredumpUrl} download data-testid="coredump-download">
              {t('coredump.download')}
            </a>
            <button type="button" className="button-secondary" disabled={clearing} onClick={() => void clear()}>
              {t(clearing ? 'coredump.clearing' : 'coredump.clear')}
            </button>
          </div>
          <p className="muted field-hint">{t('coredump.decode_hint')}</p>
        </>
      ) : null}
    </Card>
  );
}
