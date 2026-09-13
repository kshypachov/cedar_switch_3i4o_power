import { useEffect, useRef, useState } from 'react';

import { isApiFailure } from '../../api/errors';
import { pollJob } from '../../api/jobs';
import { getWiFiScan, newIdempotencyKey, scanWiFi } from '../../api/network';
import type { AccessPoint, ScanResults, Session } from '../../api/types';
import { ErrorNotice } from '../../components/ErrorNotice';
import { t } from '../../i18n';
import { useAuth } from '../../state/auth';
import { securityFor, selectable, type WifiSecurity } from './form';
import { reasonText } from './reason';
import { decodeSsid } from './ssid';

function ScanTable({
  results,
  modes,
  onPick,
}: {
  results: ScanResults;
  modes: readonly WifiSecurity[];
  onPick: (ap: AccessPoint, security: WifiSecurity) => void;
}) {
  if (!results.items.length) return <p className="muted">{t('network.scan_empty')}</p>;
  // Strongest first. One SSID may be several access points: each BSSID is its own row.
  const items = [...results.items].sort((a, b) => b.rssi_dbm - a.rssi_dbm);
  return (
    <>
      {results.truncated ? (
        <p className="notice notice-warning" data-testid="scan-truncated">
          {t('network.scan_truncated', { count: results.items.length })}
        </p>
      ) : null}
      <div className="table-scroll">
        <table data-testid="scan-results">
          <thead>
            <tr>
              <th scope="col">{t('network.ap_ssid')}</th>
              <th scope="col">{t('network.ap_rssi')}</th>
              <th scope="col">{t('network.ap_security')}</th>
              <th scope="col">{t('network.ap_channel')}</th>
              <th scope="col">{t('network.ap_bssid')}</th>
              <th scope="col">{t('network.ap_action')}</th>
            </tr>
          </thead>
          <tbody>
            {items.map((ap) => {
              const security = securityFor(ap.security, modes);
              const usable = selectable(ap, modes);
              // The exact bytes decide what is shown; bytes that are not UTF-8 appear as U+FFFD.
              const name = decodeSsid(ap.ssid_base64) ?? ap.ssid;
              return (
                <tr key={ap.bssid}>
                  <td>{name ? name : <span className="muted">{t('network.hidden_network')}</span>}</td>
                  <td>{t('value.dbm', { value: ap.rssi_dbm })}</td>
                  <td>{t(`wifi.security.${ap.security}`)}</td>
                  <td>{ap.channel}</td>
                  <td>
                    <code>{ap.bssid}</code>
                  </td>
                  <td>
                    <button
                      type="button"
                      className="button-secondary"
                      disabled={!usable}
                      onClick={() => security && onPick(ap, security)}
                    >
                      {t('network.ap_pick')}
                    </button>
                    {usable ? null : <span className="muted small">{t('network.ap_unsupported')}</span>}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </>
  );
}

export function WifiScan({
  session,
  modes,
  disabled,
  onUnavailable,
  onPick,
}: {
  session: Session;
  modes: readonly WifiSecurity[];
  disabled: boolean;
  onUnavailable: () => void;
  onPick: (ap: AccessPoint, security: WifiSecurity) => void;
}) {
  const { signedOut } = useAuth();
  const [running, setRunning] = useState(false);
  const [results, setResults] = useState<ScanResults | null>(null);
  const [problem, setProblem] = useState<string | null>(null);
  const [failure, setFailure] = useState<unknown>(null);
  // One key per scan asked for, kept for its retries and dropped when its job ends.
  const attempt = useRef<string | null>(null);
  const unmounted = useRef(new AbortController());
  useEffect(() => {
    const controller = unmounted.current;
    return () => controller.abort();
  }, []);

  async function scan() {
    attempt.current ??= newIdempotencyKey();
    setProblem(null);
    setFailure(null);
    setRunning(true);
    try {
      const accepted = await scanWiFi(session.csrf_token, attempt.current);
      const outcome = await pollJob(accepted.job_id, { signal: unmounted.current.signal });
      attempt.current = null;
      if (outcome.kind === 'session_ended') {
        signedOut('session_ended');
      } else if (outcome.job.state !== 'succeeded') {
        setProblem(t('network.scan_failed', { reason: reasonText(outcome.job.error?.code) }));
      } else {
        setResults(await getWiFiScan(accepted.job_id, unmounted.current.signal));
      }
    } catch (error) {
      if (unmounted.current.signal.aborted) return;
      if (isApiFailure(error, 'session_expired') || isApiFailure(error, 'authentication_required')) {
        signedOut('session_ended');
      } else if (isApiFailure(error, 'busy')) {
        setProblem(t('network.scan_busy'));
      } else if (isApiFailure(error, 'capability_unavailable')) {
        onUnavailable();
      } else {
        setFailure(error);
      }
    } finally {
      if (!unmounted.current.signal.aborted) setRunning(false);
    }
  }

  return (
    <div className="scan">
      <button type="button" className="button-secondary" disabled={disabled || running} onClick={() => void scan()}>
        {t('network.scan_submit')}
      </button>
      {running ? (
        <p role="status" className="muted">
          {t('network.scanning')}
        </p>
      ) : null}
      {problem ? (
        <p role="alert" className="notice notice-error">
          {problem}
        </p>
      ) : null}
      {failure ? <ErrorNotice error={failure} /> : null}
      {results ? <ScanTable results={results} modes={modes} onPick={onPick} /> : null}
    </div>
  );
}
