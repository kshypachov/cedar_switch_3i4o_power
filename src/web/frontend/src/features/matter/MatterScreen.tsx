import { type FormEvent, useEffect, useRef, useState } from 'react';

import { getCapabilities, getMatterStatus } from '../../api/device';
import { isApiFailure } from '../../api/errors';
import { pollJob } from '../../api/jobs';
import {
  closeCommissioningWindow,
  getCommissioningWindow,
  getOnboardingCodes,
  listFabrics,
  newIdempotencyKey,
  openCommissioningWindow,
} from '../../api/matter';
import type { CommissioningWindow, Fabric, OnboardingCodes, Session } from '../../api/types';
import { ErrorNotice } from '../../components/ErrorNotice';
import { formatDuration } from '../../components/format';
import { Body } from '../../components/Polled';
import { QrCode } from '../../components/QrCode';
import { Card, Facts, Loading } from '../../components/ui';
import { type MessageKey, t, tMaybe } from '../../i18n';
import { useAuth } from '../../state/auth';
import { usePolling } from '../../state/usePolling';
import { notServed, useSessionGuard } from '../../state/useSessionGuard';
import { DEFAULT_TIMEOUT, remainingNow, timeoutChoices } from './window';

const STATUS_MS = 5_000;
const WINDOW_MS = 2_000;

const yesNo = (value: boolean) => t(value ? 'value.yes' : 'value.no');
const hex16 = (value: string) => <code>{value}</code>;
const vendor = (id: number) => <code>{`0x${id.toString(16).toUpperCase().padStart(4, '0')}`}</code>;

/** A job's or a refusal's code as text. */
function reasonText(code: string | null | undefined): string {
  return (code && tMaybe(`error.${code}`)) || t('error.unknown', { code: code ?? '?' });
}

function useCountdown(window: CommissioningWindow | null): number | null {
  const [anchor, setAnchor] = useState<{ remaining: number; at: number } | null>(null);
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    setAnchor(window?.open ? { remaining: window.remaining_seconds, at: Date.now() } : null);
  }, [window]);
  useEffect(() => {
    if (!anchor) return undefined;
    const timer = setInterval(() => setNow(Date.now()), 1_000);
    return () => clearInterval(timer);
  }, [anchor]);
  return anchor ? remainingNow(anchor.remaining, anchor.at, now) : null;
}

function Codes({ window }: { window: CommissioningWindow }) {
  const [codes, setCodes] = useState<OnboardingCodes | null>(null);
  const [failure, setFailure] = useState<unknown>(null);
  // Codes do not change while one window stays open: ask once per window.
  useEffect(() => {
    if (!window.codes_available) {
      setCodes(null);
      return undefined;
    }
    const controller = new AbortController();
    getOnboardingCodes(controller.signal)
      .then((c) => setCodes(c))
      .catch((error) => {
        if (!controller.signal.aborted) setFailure(error);
      });
    return () => controller.abort();
  }, [window.codes_available, window.open, window.source]);

  if (!window.open) return <p className="muted">{t('matter.codes.window_closed')}</p>;
  if (!window.codes_available) {
    return <p className="muted">{t('matter.codes.passcode_unavailable')}</p>;
  }
  if (failure) return <ErrorNotice error={failure} />;
  if (!codes) return <Loading />;
  if (!codes.available || !codes.qr_payload || !codes.manual_pairing_code || !codes.setup_passcode) {
    return <p className="muted">{t(`matter.codes.${codes.reason ?? 'generation_failed'}` as MessageKey)}</p>;
  }
  return (
    <div className="codes">
      <QrCode payload={codes.qr_payload} />
      <Facts
        rows={[
          ['matter.manual_code', <code key="manual" data-testid="manual-code">{codes.manual_pairing_code}</code>],
          ['matter.setup_passcode', <code key="pin" data-testid="setup-passcode">{codes.setup_passcode}</code>],
          ['matter.qr_payload', <code key="qr">{codes.qr_payload}</code>],
        ]}
      />
    </div>
  );
}

function FabricTable({ items }: { items: Fabric[] }) {
  if (!items.length) return <p className="muted">{t('matter.fabrics_empty')}</p>;
  return (
    <>
      <div className="table-scroll">
        <table>
          <thead>
            <tr>
              <th scope="col">{t('matter.fabric_index')}</th>
              <th scope="col">{t('matter.fabric_label')}</th>
              <th scope="col">{t('matter.fabric_vendor')}</th>
              <th scope="col">{t('matter.fabric_id')}</th>
              <th scope="col">{t('matter.fabric_node')}</th>
            </tr>
          </thead>
          <tbody>
            {items.map((f) => (
              <tr key={f.id}>
                <td>{f.fabric_index}</td>
                {/* A label is shown as text, never as markup; empty is a real answer. */}
                <td>{f.label || <span className="muted">{t('matter.no_label')}</span>}</td>
                <td>{vendor(f.vendor_id)}</td>
                <td>{hex16(f.fabric_id)}</td>
                <td>{hex16(f.node_id)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p className="muted">{t('matter.fabrics_intro')}</p>
    </>
  );
}

export function MatterScreen({ session }: { session: Session }) {
  const { signedOut } = useAuth();
  const status = usePolling(getMatterStatus, STATUS_MS, { stopOn: notServed });
  const window = usePolling(getCommissioningWindow, WINDOW_MS, { stopOn: notServed });
  const fabrics = usePolling(listFabrics, STATUS_MS, { stopOn: notServed });
  const capabilities = usePolling(getCapabilities, STATUS_MS * 12);
  useSessionGuard(status, window, fabrics);
  const remaining = useCountdown(window.data);

  const [timeout, setTimeoutSeconds] = useState(DEFAULT_TIMEOUT);
  const [running, setRunning] = useState<'opening' | 'closing' | null>(null);
  const [problem, setProblem] = useState<string | null>(null);
  const [failure, setFailure] = useState<unknown>(null);
  // One key per intended request, kept for every retry of it - a lost answer,
  // a timeout, a refusal - and dropped once its job has finished, so the next
  // open is a new request (see AccessScreen).
  const attempt = useRef<{ key: string; action: string } | null>(null);
  const unmounted = useRef(new AbortController());
  useEffect(() => {
    const controller = unmounted.current;
    return () => controller.abort();
  }, []);

  const limits = capabilities.data?.limits;
  const choices = timeoutChoices(limits?.commissioning_min_seconds ?? 180, limits?.commissioning_max_seconds ?? 900);
  const ready = status.data?.state === 'ready';

  async function run(action: 'open' | 'close') {
    const intended = action === 'open' ? `open:${timeout}` : 'close';
    if (attempt.current?.action !== intended) attempt.current = { key: newIdempotencyKey(), action: intended };
    setProblem(null);
    setFailure(null);
    setRunning(action === 'open' ? 'opening' : 'closing');
    try {
      const accepted =
        action === 'open'
          ? await openCommissioningWindow(session.csrf_token, attempt.current.key, timeout)
          : await closeCommissioningWindow(session.csrf_token, attempt.current.key);
      const outcome = await pollJob(accepted.job_id, { signal: unmounted.current.signal });
      attempt.current = null;
      if (outcome.kind === 'session_ended') {
        signedOut('session_ended');
      } else if (outcome.job.state !== 'succeeded') {
        setProblem(t('matter.request_failed', { reason: reasonText(outcome.job.error?.code) }));
      }
    } catch (error) {
      if (unmounted.current.signal.aborted) return;
      if (isApiFailure(error, 'session_expired') || isApiFailure(error, 'authentication_required')) {
        signedOut('session_ended');
        return;
      }
      if (isApiFailure(error, 'invalid_state')) {
        setProblem(t('matter.window_already_open'));
      } else {
        setFailure(error);
      }
    } finally {
      if (!unmounted.current.signal.aborted) setRunning(null);
    }
  }

  function submitOpen(event: FormEvent) {
    event.preventDefault();
    void run('open');
  }

  return (
    <main>
      <h1>{t('matter.title')}</h1>
      <Card title="matter.status">
        <Body
          polled={status}
          render={(s) => (
            <>
              <Facts
                rows={[
                  ['overview.matter_state', t(`matter.${s.state}`)],
                  ['matter.commissioned', yesNo(s.commissioned)],
                  ['overview.fabrics', String(s.fabric_count)],
                ]}
              />
              {s.state === 'failed' ? <p className="notice notice-error">{t('matter.failed_notice')}</p> : null}
              {s.state === 'not_ready' || s.state === 'starting' ? (
                <p className="notice">{t('matter.not_ready_notice')}</p>
              ) : null}
            </>
          )}
        />
      </Card>
      <Card title="matter.window">
        <Body
          polled={window}
          render={(w) => (
            <>
              <p>{t('matter.window_intro')}</p>
              <Facts
                rows={[
                  ['matter.window_state', t(w.open ? 'matter.window_open' : 'matter.window_closed')],
                  ...(w.open
                    ? ([
                        ['matter.window_mode', w.mode ? t(`matter.mode.${w.mode}`) : t('value.unknown')],
                        ['matter.window_source', w.source ? t(`matter.source.${w.source}`) : t('value.unknown')],
                        [
                          'matter.window_remaining',
                          w.source === 'web' && remaining !== null ? (
                            <span key="remaining" data-testid="window-remaining">{formatDuration(remaining)}</span>
                          ) : (
                            t('matter.window_remaining_unknown')
                          ),
                        ],
                      ] as [MessageKey, React.ReactNode][])
                    : []),
                ]}
              />
              {w.open ? (
                <button type="button" className="button-secondary" disabled={!ready || running !== null} onClick={() => void run('close')}>
                  {t('matter.close_submit')}
                </button>
              ) : (
                <form onSubmit={submitOpen} noValidate>
                  <div className="field">
                    <label htmlFor="window-timeout">{t('matter.timeout_label')}</label>
                    <select
                      id="window-timeout"
                      value={timeout}
                      onChange={(e) => setTimeoutSeconds(Number(e.target.value))}
                      disabled={running !== null}
                    >
                      {choices.map((s) => (
                        <option key={s} value={s}>
                          {formatDuration(s)}
                        </option>
                      ))}
                    </select>
                  </div>
                  <button type="submit" disabled={!ready || running !== null}>
                    {t('matter.open_submit')}
                  </button>
                </form>
              )}
              {running ? (
                <p role="status" className="muted">
                  {t(running === 'opening' ? 'matter.opening' : 'matter.closing')}
                </p>
              ) : null}
              {problem ? (
                <p role="alert" className="notice notice-error">
                  {problem}
                </p>
              ) : null}
              {failure ? <ErrorNotice error={failure} /> : null}
            </>
          )}
        />
      </Card>
      <Card title="matter.codes">
        {window.data ? <Codes window={window.data} /> : <Body polled={window} render={() => null} />}
      </Card>
      <Card title="matter.fabrics">
        <Body polled={fabrics} render={(f) => <FabricTable items={f.items} />} />
      </Card>
    </main>
  );
}
