import { useEffect, useRef, useState } from 'react';

import { ApiFailure, isApiFailure } from '../../api/errors';
import { pollJob } from '../../api/jobs';
import {
  applyNetworkTransaction,
  confirmNetworkTransaction,
  newIdempotencyKey,
  rollbackNetworkTransaction,
} from '../../api/network';
import type { Capabilities, NetworkConfigOutput, NetworkStatus, NetworkTransaction, Session } from '../../api/types';
import { ErrorNotice } from '../../components/ErrorNotice';
import { formatDuration } from '../../components/format';
import { Card, Facts, Loading } from '../../components/ui';
import { type MessageKey, t } from '../../i18n';
import { useAuth } from '../../state/auth';
import { remainingNow } from '../matter/window';
import type { Iface } from './form';
import { reasonText } from './reason';
import {
  confirmTimeoutChoices,
  currentIpv4,
  DEFAULT_CONFIRM_TIMEOUT,
  type FutureAddress,
  futureAddress,
  PENDING_STATES,
  reconnectLinks,
  TXN_PARAM,
} from './transaction';

type Action = 'apply' | 'confirm' | 'rollback';

const RUNNING: Record<Action, MessageKey> = {
  apply: 'network.apply_running',
  confirm: 'network.confirm_running',
  rollback: 'network.rollback_running',
};

/**
 * Seconds left as the device last said them, counted down locally only until
 * the next poll replaces the anchor: the deadline is the device's, and a tab
 * that slept or a clock that jumped must not show a different one.
 */
function useCountdown(tx: NetworkTransaction | null): number | null {
  const [anchor, setAnchor] = useState<{ remaining: number; at: number } | null>(null);
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    setAnchor(tx && tx.remaining_seconds !== null ? { remaining: tx.remaining_seconds, at: Date.now() } : null);
    setNow(Date.now());
  }, [tx]);
  useEffect(() => {
    if (!anchor) return undefined;
    const timer = setInterval(() => setNow(Date.now()), 1_000);
    return () => clearInterval(timer);
  }, [anchor]);
  return anchor ? remainingNow(anchor.remaining, anchor.at, now) : null;
}

export function describeAddress(future: FutureAddress): string {
  switch (future.kind) {
    case 'disabled':
      return t('network.future_disabled');
    case 'dhcp':
      return t('network.future_dhcp');
    case 'static':
      return future.prefix === null ? future.address : `${future.address}/${future.prefix}`;
  }
}

function AddressComparison({ candidate, status }: { candidate: NetworkConfigOutput; status: NetworkStatus | null }) {
  const row = (iface: Iface, label: MessageKey) => {
    const now = currentIpv4(status?.interfaces.find((i) => i.id === iface));
    return (
      <tr key={iface}>
        <th scope="row">{t(label)}</th>
        <td data-testid={`current-${iface}`}>{now.length ? now.join(', ') : t('value.none')}</td>
        <td data-testid={`future-${iface}`}>{describeAddress(futureAddress(candidate, iface))}</td>
      </tr>
    );
  };
  return (
    <div className="table-scroll">
      <table>
        <thead>
          <tr>
            <th scope="col">{t('network.interface')}</th>
            <th scope="col">{t('network.address_now')}</th>
            <th scope="col">{t('network.address_after')}</th>
          </tr>
        </thead>
        <tbody>
          {row('ethernet', 'overview.ethernet')}
          {row('wifi', 'overview.wifi')}
        </tbody>
      </table>
    </div>
  );
}

function Reconnect({ tx }: { tx: NetworkTransaction }) {
  const links = reconnectLinks(tx.reconnect_urls, tx.id);
  return (
    <div className="reconnect">
      <p>{t('network.reconnect_intro')}</p>
      {links.length ? (
        <ul className="plain">
          {links.map((href) => (
            <li key={href}>
              <a href={href} data-testid="reconnect-link">
                {href}
              </a>
            </li>
          ))}
        </ul>
      ) : (
        <p className="muted" data-testid="reconnect-dhcp">
          {t('network.reconnect_dhcp', { path: `/network?${TXN_PARAM}=${tx.id}` })}
        </p>
      )}
    </div>
  );
}

function Outcome({ tx }: { tx: NetworkTransaction }) {
  const [text, tone]: [string, string] =
    tx.state === 'committed'
      ? [t('network.committed_notice'), 'notice']
      : tx.state === 'rolled_back'
        ? tx.error?.code === 'resource_expired'
          ? [t('network.timeout_notice'), 'notice notice-warning']
          : [t('network.rolled_back_notice'), 'notice']
        : tx.state === 'expired'
          ? [t('network.expired_notice'), 'notice notice-warning']
          : [t('network.failed_notice', { reason: reasonText(tx.error?.code) }), 'notice notice-error'];
  return (
    <p className={tone} data-testid="transaction-outcome">
      {text}
    </p>
  );
}

export function TransactionCard({
  id,
  tx,
  error,
  session,
  status,
  capabilities,
  onRefresh,
  onClose,
}: {
  id: string;
  tx: NetworkTransaction | null;
  /** The last failure reading the transaction, if the latest read failed. */
  error: unknown;
  session: Session;
  status: NetworkStatus | null;
  capabilities: Capabilities | null;
  onRefresh: () => void;
  onClose: () => void;
}) {
  const { signedOut } = useAuth();
  const remaining = useCountdown(tx);
  const limits = capabilities?.limits;
  const choices = confirmTimeoutChoices(limits?.network_confirm_min_seconds ?? 60, limits?.network_confirm_max_seconds ?? 300);
  const [timeout, setTimeoutSeconds] = useState(choices.includes(DEFAULT_CONFIRM_TIMEOUT) ? DEFAULT_CONFIRM_TIMEOUT : choices[0]!);
  const [running, setRunning] = useState<Action | null>(null);
  const [problem, setProblem] = useState<string | null>(null);
  const [failure, setFailure] = useState<unknown>(null);
  // One key per intended action on this transaction, kept for every retry of
  // it - including a confirm refused while the new settings are not working
  // yet, which the device does not record - and dropped once it is answered.
  const attempt = useRef<{ key: string; action: string } | null>(null);
  const unmounted = useRef(new AbortController());
  useEffect(() => {
    const controller = unmounted.current;
    return () => controller.abort();
  }, []);

  if (!tx) {
    const missing = isApiFailure(error, 'not_found');
    return (
      <Card title="network.transaction">
        {missing ? (
          <>
            <p className="notice notice-warning" data-testid="transaction-missing">
              {t('network.transaction_missing', { id })}
            </p>
            <button type="button" className="button-secondary" onClick={onClose}>
              {t('network.close_transaction')}
            </button>
          </>
        ) : error ? (
          <ErrorNotice error={error} />
        ) : (
          <Loading />
        )}
      </Card>
    );
  }

  const current = tx;
  const pending = PENDING_STATES.has(current.state);
  // A pending transaction the page can no longer read: most likely the address changed.
  const lost = pending && error instanceof ApiFailure && (error.kind === 'network' || error.kind === 'timeout');

  async function run(action: Action) {
    const intended = action === 'apply' ? `apply:${current.id}:${timeout}` : `${action}:${current.id}`;
    if (attempt.current?.action !== intended) attempt.current = { key: newIdempotencyKey(), action: intended };
    const key = attempt.current.key;
    setProblem(null);
    setFailure(null);
    setRunning(action);
    try {
      if (action === 'apply') {
        // The apply job parks at waiting_confirmation; the transaction, not the
        // job, is what shows the change and its deadline.
        await applyNetworkTransaction(session.csrf_token, key, current.id, timeout);
        attempt.current = null;
        return;
      }
      const accepted =
        action === 'confirm'
          ? await confirmNetworkTransaction(session.csrf_token, key, current.id)
          : await rollbackNetworkTransaction(session.csrf_token, key, current.id);
      const outcome = await pollJob(accepted.job_id, { signal: unmounted.current.signal });
      attempt.current = null;
      if (outcome.kind === 'session_ended') {
        signedOut('session_ended');
      } else if (outcome.job.state !== 'succeeded') {
        setProblem(t('network.request_failed', { reason: reasonText(outcome.job.error?.code) }));
      }
    } catch (error) {
      if (unmounted.current.signal.aborted) return;
      if (isApiFailure(error, 'session_expired') || isApiFailure(error, 'authentication_required')) {
        signedOut('session_ended');
      } else if (action === 'confirm' && isApiFailure(error, 'invalid_state')) {
        setProblem(t('network.confirm_not_ready'));
      } else if (isApiFailure(error, 'invalid_state') || isApiFailure(error, 'not_found')) {
        setProblem(t('network.transaction_moved_on'));
      } else {
        setFailure(error);
      }
    } finally {
      if (!unmounted.current.signal.aborted) {
        setRunning(null);
        onRefresh();
      }
    }
  }

  const rows: [MessageKey, React.ReactNode][] = [
    ['network.transaction_id', <code key="id" data-testid="transaction-id">{current.id}</code>],
    ['network.transaction_state', <span key="state" data-testid="transaction-state">{t(`network.tx.${current.state}`)}</span>],
  ];
  if (remaining !== null && pending && current.state !== 'rolling_back') {
    rows.push([
      current.state === 'staged' ? 'network.staged_remaining' : 'network.confirm_remaining',
      <span key="remaining" data-testid="transaction-remaining">
        {formatDuration(remaining)}
      </span>,
    ]);
  }

  return (
    <Card title="network.transaction">
      <Facts rows={rows} />
      {lost ? <p className="notice notice-warning">{t('network.connection_lost')}</p> : null}

      {current.state === 'staged' ? (
        <>
          <p>{t('network.staged_intro')}</p>
          <AddressComparison candidate={current.candidate} status={status} />
          <div className="field">
            <label htmlFor="confirm-timeout">{t('network.confirm_timeout_label')}</label>
            <select
              id="confirm-timeout"
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
          <div className="actions">
            <button type="button" disabled={running !== null} onClick={() => void run('apply')}>
              {t('network.apply_submit')}
            </button>
            <button type="button" className="button-secondary" disabled={running !== null} onClick={() => void run('rollback')}>
              {t('network.discard_submit')}
            </button>
          </div>
        </>
      ) : null}

      {current.state === 'applying' || current.state === 'awaiting_confirmation' ? (
        <>
          <p>{t(current.state === 'applying' ? 'network.applying_intro' : 'network.awaiting_intro')}</p>
          <AddressComparison candidate={current.candidate} status={status} />
          <Reconnect tx={current} />
          <div className="actions">
            <button
              type="button"
              disabled={current.state !== 'awaiting_confirmation' || running !== null}
              onClick={() => void run('confirm')}
            >
              {t('network.confirm_submit')}
            </button>
            <button type="button" className="button-secondary" disabled={running !== null} onClick={() => void run('rollback')}>
              {t('network.rollback_submit')}
            </button>
          </div>
        </>
      ) : null}

      {current.state === 'rolling_back' ? (
        <p role="status" className="muted">
          {t('network.rolling_back_intro')}
        </p>
      ) : null}

      {!pending ? (
        <>
          <Outcome tx={current} />
          <button type="button" className="button-secondary" onClick={onClose}>
            {t('network.close_transaction')}
          </button>
        </>
      ) : null}

      {running ? (
        <p role="status" className="muted">
          {t(RUNNING[running])}
        </p>
      ) : null}
      {problem ? (
        <p role="alert" className="notice notice-error">
          {problem}
        </p>
      ) : null}
      {failure ? <ErrorNotice error={failure} /> : null}
    </Card>
  );
}
