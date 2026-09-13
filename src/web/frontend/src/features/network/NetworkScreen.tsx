import { type FormEvent, useCallback, useEffect, useRef, useState } from 'react';

import { getCapabilities, getNetworkStatus } from '../../api/device';
import { type ErrorField, isApiFailure } from '../../api/errors';
import { getNetworkConfig, getNetworkTransaction, newIdempotencyKey, stageNetworkConfig } from '../../api/network';
import type {
  AccessPoint,
  InterfaceStatus,
  NetworkConfigOutput,
  NetworkConfigResponse,
  NetworkTransaction,
  Session,
} from '../../api/types';
import { describeField, ErrorNotice } from '../../components/ErrorNotice';
import { Body } from '../../components/Polled';
import { Card, Facts } from '../../components/ui';
import { type MessageKey, t } from '../../i18n';
import { useAuth } from '../../state/auth';
import { usePolling } from '../../state/usePolling';
import { notServed, useSessionGuard } from '../../state/useSessionGuard';
import { CheckboxField, SelectField, TextField } from './fields';
import {
  type FieldId,
  formFromConfig,
  type Iface,
  type Ipv4Form,
  keepsPassword,
  type NetworkForm,
  needsManualDnsHint,
  placeFieldErrors,
  prepare,
  withAccessPoint,
  withEnabled,
  withSsidText,
  type WifiSecurity,
  wifiUnavailable,
} from './form';
import { reasonText } from './reason';
import { encodeSsid, SSID_MAX_BYTES, ssidByteLength, decodeSsid } from './ssid';
import { describeAddress, TransactionCard } from './TransactionCard';
import { futureAddress, PENDING_STATES, transactionFromSearch } from './transaction';
import { WifiScan } from './WifiScan';

const STATUS_MS = 5_000;
/** An open transaction is an active operation: the contract's 500-1000 ms. */
const TRANSACTION_MS = 1_000;

const yesNo = (value: boolean) => t(value ? 'value.yes' : 'value.no');
const IFACES: Iface[] = ['ethernet', 'wifi'];
const IFACE_LABEL: Record<Iface, MessageKey> = { ethernet: 'overview.ethernet', wifi: 'overview.wifi' };

type StageProblem = 'validation' | 'stale' | 'busy' | null;

function RuntimeInterface({ iface }: { iface: InterfaceStatus }) {
  const rows: [MessageKey, React.ReactNode][] = [
    ['network.state', t(`interface.${iface.state}`)],
    ['overview.link', yesNo(iface.link_up)],
    [
      'overview.addresses',
      iface.addresses.length ? (
        <ul className="plain" key="addresses">
          {iface.addresses.map((a) => (
            <li key={a.address}>
              <code>
                {a.address}/{a.prefix_length}
              </code>{' '}
              <span className="muted">{t(`address.source.${a.source}`)}</span>
            </li>
          ))}
        </ul>
      ) : (
        t('value.none')
      ),
    ],
    ['network.mac', <code key="mac">{iface.mac_address}</code>],
  ];
  if (iface.id === 'wifi') {
    // SSIDs are shown as text, never as markup (plan section 4).
    rows.push(['overview.ssid', iface.ssid ?? t('value.none')]);
    rows.push(['overview.rssi', iface.rssi_dbm === null ? t('value.none') : t('value.dbm', { value: iface.rssi_dbm })]);
  }
  if (iface.error) rows.push(['network.interface_error', reasonText(iface.error.code)]);
  return (
    <div className="subcard" data-testid={`runtime-${iface.id}`}>
      <h3>{t(IFACE_LABEL[iface.id])}</h3>
      <Facts rows={rows} />
    </div>
  );
}

function CommittedFacts({ config }: { config: NetworkConfigOutput }) {
  const { wifi } = config.interfaces;
  const wifiText = wifi.enabled
    ? t('network.wifi_summary', {
        ssid: decodeSsid(wifi.ssid_base64) ?? '',
        security: t(`wifi.security.${wifi.security}`),
        address: describeAddress(futureAddress(config, 'wifi')),
      })
    : t('network.future_disabled');
  return (
    <Facts
      rows={[
        ['overview.ethernet', describeAddress(futureAddress(config, 'ethernet'))],
        ['overview.wifi', wifiText],
        ['network.password_set', yesNo(wifi.password_set)],
        ['network.preferred_interface', t(IFACE_LABEL[config.preferred_interface])],
        ['network.dns', config.dns.mode === 'automatic' ? t('network.dns_mode.automatic') : config.dns.servers.join(', ')],
      ]}
    />
  );
}

function Ipv4Fields({
  iface,
  value,
  onChange,
  error,
}: {
  iface: Iface;
  value: Ipv4Form;
  onChange: (value: Ipv4Form) => void;
  error: (id: FieldId) => string | null;
}) {
  return (
    <>
      <SelectField
        id={`${iface}-mode`}
        label="network.ipv4_mode"
        value={value.mode}
        options={[
          { value: 'dhcp', text: t('network.ipv4_mode.dhcp') },
          { value: 'static', text: t('network.ipv4_mode.static') },
        ]}
        onChange={(mode) => onChange({ ...value, mode })}
        error={error(`${iface}-mode`)}
      />
      {value.mode === 'static' ? (
        <div className="field-row">
          <TextField
            id={`${iface}-address`}
            label="network.address"
            value={value.address}
            inputMode="decimal"
            onChange={(address) => onChange({ ...value, address })}
            error={error(`${iface}-address`)}
          />
          <TextField
            id={`${iface}-prefix`}
            label="network.prefix"
            value={value.prefix}
            inputMode="numeric"
            hint={t('network.prefix_hint')}
            onChange={(prefix) => onChange({ ...value, prefix })}
            error={error(`${iface}-prefix`)}
          />
          <TextField
            id={`${iface}-gateway`}
            label="network.gateway"
            value={value.gateway}
            inputMode="decimal"
            hint={t('network.gateway_hint')}
            onChange={(gateway) => onChange({ ...value, gateway })}
            error={error(`${iface}-gateway`)}
          />
        </div>
      ) : null}
    </>
  );
}

/** Reads the transaction while it is open; unmounted once it has ended, so a finished one is not polled. */
function TransactionWatcher({
  id,
  onUpdate,
  onError,
}: {
  id: string;
  onUpdate: (tx: NetworkTransaction) => void;
  onError: (error: unknown) => void;
}) {
  const polled = usePolling((signal) => getNetworkTransaction(id, signal), TRANSACTION_MS, {
    stopOn: (error) => isApiFailure(error, 'not_found'),
  });
  useSessionGuard(polled);
  useEffect(() => {
    if (polled.data) onUpdate(polled.data);
  }, [polled.data, onUpdate]);
  useEffect(() => {
    onError(polled.error);
  }, [polled.error, onError]);
  return null;
}

export function NetworkScreen({ session }: { session: Session }) {
  const { signedOut } = useAuth();
  const status = usePolling(getNetworkStatus, STATUS_MS, { stopOn: notServed });
  const config = usePolling(getNetworkConfig, STATUS_MS, { stopOn: notServed });
  const capabilities = usePolling(getCapabilities, STATUS_MS * 12);
  useSessionGuard(status, config);

  // The open transaction: from the reconnect link's parameter, from the
  // configuration's pending id (a reload, a lost answer), or just staged here.
  const [txnId, setTxnId] = useState<string | null>(() => transactionFromSearch(window.location.search));
  const [tx, setTx] = useState<NetworkTransaction | null>(null);
  const [txError, setTxError] = useState<unknown>(null);
  const dismissed = useRef(new Set<string>());

  // The form is made once from the configuration and then belongs to the
  // person editing it; `base` is the configuration it was made from.
  const [base, setBase] = useState<NetworkConfigResponse | null>(null);
  const [form, setForm] = useState<NetworkForm | null>(null);
  const [errors, setErrors] = useState<Map<FieldId, string>>(new Map());
  const [unplaced, setUnplaced] = useState<ErrorField[]>([]);
  const [stageProblem, setStageProblem] = useState<StageProblem>(null);
  const [failure, setFailure] = useState<unknown>(null);
  const [staging, setStaging] = useState(false);
  const [radioGone, setRadioGone] = useState(false);
  // One key per candidate body: a retry of the same candidate gets the same
  // transaction back, a changed one is a new request.
  const attempt = useRef<{ key: string; body: string } | null>(null);
  const committedSeen = useRef<string | null>(null);
  const unmounted = useRef(new AbortController());
  useEffect(() => {
    const controller = unmounted.current;
    return () => controller.abort();
  }, []);

  const reset = useCallback((fresh: NetworkConfigResponse) => {
    setBase(fresh);
    setForm(formFromConfig(fresh.config));
    setErrors(new Map());
    setUnplaced([]);
    setStageProblem(null);
    setFailure(null);
  }, []);

  useEffect(() => {
    if (!base && config.data) reset(config.data);
  }, [base, config.data, reset]);

  const openTransaction = useCallback((id: string) => {
    setTx(null);
    setTxError(null);
    setTxnId(id);
  }, []);

  useEffect(() => {
    const pending = config.data?.pending_transaction_id;
    if (!txnId && pending && !dismissed.current.has(pending)) openTransaction(pending);
  }, [config.data, txnId, openTransaction]);

  async function reread() {
    try {
      reset(await getNetworkConfig(unmounted.current.signal));
    } catch (error) {
      if (!unmounted.current.signal.aborted) setFailure(error);
    }
  }

  // A committed change moved the revision on: the form starts again from it.
  useEffect(() => {
    if (tx?.state === 'committed' && committedSeen.current !== tx.id) {
      committedSeen.current = tx.id;
      void reread();
    }
  }, [tx]);

  const onUpdate = useCallback((fresh: NetworkTransaction) => setTx(fresh), []);
  const onError = useCallback((error: unknown) => setTxError(error), []);

  async function refreshTransaction() {
    if (!txnId) return;
    try {
      setTx(await getNetworkTransaction(txnId, unmounted.current.signal));
      setTxError(null);
    } catch {
      // The watcher reports what it cannot read.
    }
  }

  function closeTransaction() {
    if (txnId) dismissed.current.add(txnId);
    setTxnId(null);
    setTx(null);
    setTxError(null);
    if (transactionFromSearch(window.location.search)) window.history.replaceState(null, '', window.location.pathname);
  }

  async function openPending() {
    try {
      const fresh = await getNetworkConfig(unmounted.current.signal);
      if (fresh.pending_transaction_id) {
        setStageProblem(null);
        dismissed.current.delete(fresh.pending_transaction_id);
        openTransaction(fresh.pending_transaction_id);
      } else {
        reset(fresh);
      }
    } catch (error) {
      if (!unmounted.current.signal.aborted) setFailure(error);
    }
  }

  const txFinished = tx !== null && tx.id === txnId && !PENDING_STATES.has(tx.state);
  const txMissing = isApiFailure(txError, 'not_found');
  const locked = txnId !== null && !txFinished && !txMissing;
  const wifiStatus = status.data?.interfaces.find((i) => i.id === 'wifi');
  const unavailable = radioGone || wifiUnavailable(wifiStatus);
  const offered: WifiSecurity[] = capabilities.data?.wifi_security_modes ?? [];
  const modes: WifiSecurity[] = form && !offered.includes(form.wifi.security) ? [...offered, form.wifi.security] : offered;
  const error = (id: FieldId) => errors.get(id) ?? null;

  const update = (change: (f: NetworkForm) => NetworkForm) => setForm((f) => (f ? change(f) : f));

  async function stage(event: FormEvent) {
    event.preventDefault();
    if (!form || !base) return;
    setStageProblem(null);
    setFailure(null);
    setUnplaced([]);
    const prepared = prepare(form, base);
    if (prepared.problems) {
      setErrors(new Map([...prepared.problems].map(([id, key]) => [id, t(key)])));
      return;
    }
    setErrors(new Map());
    const body = JSON.stringify(prepared.request);
    if (attempt.current?.body !== body) attempt.current = { key: newIdempotencyKey(), body };
    setStaging(true);
    try {
      const staged = await stageNetworkConfig(session.csrf_token, attempt.current.key, prepared.request);
      attempt.current = null;
      setTx(staged);
      setTxError(null);
      setTxnId(staged.id);
    } catch (error) {
      if (unmounted.current.signal.aborted) return;
      if (isApiFailure(error, 'session_expired') || isApiFailure(error, 'authentication_required')) {
        signedOut('session_ended');
      } else if (isApiFailure(error, 'validation_failed')) {
        const { placed, unplaced: rest } = placeFieldErrors(error.detail?.fields ?? []);
        setErrors(new Map([...placed].map(([id, code]) => [id, describeField(code)])));
        setUnplaced(rest);
        setStageProblem('validation');
      } else if (isApiFailure(error, 'stale_revision')) {
        setStageProblem('stale');
      } else if (isApiFailure(error, 'busy')) {
        setStageProblem('busy');
      } else {
        setFailure(error);
      }
    } finally {
      if (!unmounted.current.signal.aborted) setStaging(false);
    }
  }

  function pick(ap: AccessPoint, security: WifiSecurity) {
    update((f) => withAccessPoint(f, ap, security));
    // A hidden network has no name to pick: it is typed.
    if (!ap.ssid_base64) document.getElementById('wifi-ssid')?.focus();
  }

  const storedWifi = base?.config.interfaces.wifi;
  const ssidPreserved = form ? form.wifi.ssidBase64 !== encodeSsid(form.wifi.ssid) : false;

  return (
    <main>
      <h1>{t('network.title')}</h1>
      <div className="grid">
        <Card title="network.runtime">
          <Body
            polled={status}
            render={(s) => (
              <>
                {s.interfaces.map((iface) => (
                  <RuntimeInterface key={iface.id} iface={iface} />
                ))}
                <Facts
                  rows={[
                    ['network.default_interface', s.default_interface ? t(IFACE_LABEL[s.default_interface]) : t('value.none')],
                    [
                      'network.dns_in_force',
                      s.dns_servers.length ? (
                        <ul className="plain" key="dns">
                          {s.dns_servers.map((server) => (
                            <li key={server}>
                              <code>{server}</code>
                            </li>
                          ))}
                        </ul>
                      ) : (
                        t('value.none')
                      ),
                    ],
                  ]}
                />
              </>
            )}
          />
        </Card>
        <Card title="network.committed">
          <Body
            polled={config}
            render={(c) => (
              <>
                <Facts
                  rows={[
                    ['network.revision', <span key="revision" data-testid="config-revision">{c.revision}</span>],
                    [
                      'network.pending_transaction',
                      c.pending_transaction_id ? <code key="pending">{c.pending_transaction_id}</code> : t('value.none'),
                    ],
                  ]}
                />
                <CommittedFacts config={c.config} />
                {c.pending_transaction_id && c.pending_transaction_id !== txnId ? (
                  <button type="button" className="button-secondary" onClick={() => openTransaction(c.pending_transaction_id!)}>
                    {t('network.open_transaction')}
                  </button>
                ) : null}
                {base && c.revision !== base.revision && !locked ? (
                  <div className="notice notice-warning">
                    <p>{t('network.config_changed', { revision: c.revision })}</p>
                    <button type="button" className="button-secondary" onClick={() => void reread()}>
                      {t('network.reread')}
                    </button>
                  </div>
                ) : null}
              </>
            )}
          />
        </Card>
      </div>

      {txnId ? (
        <>
          {!txFinished && !txMissing ? (
            <TransactionWatcher key={`watch:${txnId}`} id={txnId} onUpdate={onUpdate} onError={onError} />
          ) : null}
          <TransactionCard
            key={`card:${txnId}`}
            id={txnId}
            tx={tx?.id === txnId ? tx : null}
            error={txError}
            session={session}
            status={status.data}
            capabilities={capabilities.data}
            onRefresh={() => void refreshTransaction()}
            onClose={closeTransaction}
          />
        </>
      ) : null}

      {!form || !storedWifi ? (
        <Card title="network.edit">
          <Body polled={config} render={() => null} />
        </Card>
      ) : (
        <form onSubmit={stage} noValidate>
          <p className="muted">{t('network.edit_intro')}</p>
          <fieldset className="bare" disabled={locked || staging}>
            <Card title="network.ethernet_form">
              <CheckboxField
                id="ethernet-enabled"
                label="network.ethernet_enabled"
                checked={form.ethernet.enabled}
                onChange={(on) => update((f) => withEnabled(f, 'ethernet', on))}
                error={error('ethernet-enabled')}
              />
              <Ipv4Fields
                iface="ethernet"
                value={form.ethernet.ipv4}
                onChange={(ipv4) => update((f) => ({ ...f, ethernet: { ...f.ethernet, ipv4 } }))}
                error={error}
              />
            </Card>

            <Card title="network.wifi_form">
              {unavailable ? (
                <p className="notice notice-error" data-testid="wifi-unavailable">
                  {t('network.wifi_unavailable')}
                </p>
              ) : null}
              <CheckboxField
                id="wifi-enabled"
                label="network.wifi_enabled"
                checked={form.wifi.enabled}
                // Switching Wi-Fi off stays possible; switching it on needs a radio.
                disabled={unavailable && !form.wifi.enabled}
                onChange={(on) => update((f) => withEnabled(f, 'wifi', on))}
                error={error('wifi-enabled')}
              />
              <WifiScan
                session={session}
                modes={offered}
                disabled={unavailable}
                onUnavailable={() => setRadioGone(true)}
                onPick={pick}
              />
              <TextField
                id="wifi-ssid"
                label="network.ssid"
                value={form.wifi.ssid}
                onChange={(text) => update((f) => ({ ...f, wifi: withSsidText(f.wifi, text) }))}
                hint={
                  <>
                    {t('network.ssid_bytes', { n: ssidByteLength(form.wifi.ssidBase64), max: SSID_MAX_BYTES })}
                    {ssidPreserved ? ` ${t('network.ssid_preserved')}` : null}
                  </>
                }
                error={error('wifi-ssid')}
              />
              <CheckboxField
                id="wifi-hidden"
                label="network.hidden"
                checked={form.wifi.hidden}
                hint={t('network.hidden_hint')}
                onChange={(hidden) => update((f) => ({ ...f, wifi: { ...f.wifi, hidden } }))}
                error={error('wifi-hidden')}
              />
              <SelectField
                id="wifi-security"
                label="network.security"
                value={form.wifi.security}
                options={modes.map((m) => ({ value: m, text: t(`wifi.security.${m}`) }))}
                onChange={(security) => update((f) => ({ ...f, wifi: { ...f.wifi, security } }))}
                error={error('wifi-security')}
              />
              {form.wifi.security !== 'open' ? (
                <TextField
                  id="wifi-password"
                  label="network.password"
                  type="password"
                  autoComplete="new-password"
                  value={form.wifi.password}
                  hint={t(keepsPassword(form.wifi, storedWifi) ? 'network.password_keep_hint' : 'network.password_new_hint')}
                  onChange={(password) => update((f) => ({ ...f, wifi: { ...f.wifi, password } }))}
                  error={error('wifi-password')}
                />
              ) : (
                <>
                  {storedWifi.password_set ? <p className="muted">{t('network.open_clears_password')}</p> : null}
                  {error('wifi-password') ? <p className="field-error">{error('wifi-password')}</p> : null}
                </>
              )}
              <Ipv4Fields
                iface="wifi"
                value={form.wifi.ipv4}
                onChange={(ipv4) => update((f) => ({ ...f, wifi: { ...f.wifi, ipv4 } }))}
                error={error}
              />
            </Card>

            <Card title="network.dns_form">
              <SelectField
                id="preferred-interface"
                label="network.preferred_interface"
                value={form.preferred}
                options={IFACES.filter((i) => form[i].enabled || form.preferred === i).map((i) => ({
                  value: i,
                  text: t(IFACE_LABEL[i]),
                }))}
                onChange={(preferred) => update((f) => ({ ...f, preferred }))}
                error={error('preferred-interface')}
              />
              <SelectField
                id="dns-mode"
                label="network.dns_mode"
                value={form.dns.mode}
                options={[
                  { value: 'automatic', text: t('network.dns_mode.automatic') },
                  { value: 'manual', text: t('network.dns_mode.manual') },
                ]}
                onChange={(mode) => update((f) => ({ ...f, dns: { ...f.dns, mode } }))}
                error={error('dns-mode')}
              />
              {form.dns.mode === 'manual' ? (
                <div className="field-row">
                  {([0, 1] as const).map((i) => (
                    <TextField
                      key={i}
                      id={`dns-server-${i}`}
                      label={i === 0 ? 'network.dns_server_1' : 'network.dns_server_2'}
                      value={form.dns.servers[i]}
                      onChange={(server) =>
                        update((f) => {
                          const servers: [string, string] = [...f.dns.servers];
                          servers[i] = server;
                          return { ...f, dns: { ...f.dns, servers } };
                        })
                      }
                      error={error(`dns-server-${i}`)}
                    />
                  ))}
                </div>
              ) : null}
              {needsManualDnsHint(form) ? (
                <p className="notice notice-warning" data-testid="dns-hint">
                  {t('network.dns_static_hint')}
                </p>
              ) : null}
            </Card>
          </fieldset>

          {locked ? <p className="muted">{t('network.form_locked')}</p> : null}
          {stageProblem === 'validation' ? (
            <div role="alert" className="notice notice-error">
              <p>{t('error.validation_failed')}</p>
              {unplaced.length ? (
                <ul className="plain" data-testid="unplaced-fields">
                  {unplaced.map((f) => (
                    <li key={`${f.path}:${f.code}`}>
                      <code>{f.path}</code> {describeField(f.code)}
                    </li>
                  ))}
                </ul>
              ) : null}
            </div>
          ) : null}
          {stageProblem === 'stale' ? (
            <div role="alert" className="notice notice-error">
              <p>{t('network.stale_revision')}</p>
              <button type="button" className="button-secondary" onClick={() => void reread()}>
                {t('network.reread')}
              </button>
            </div>
          ) : null}
          {stageProblem === 'busy' ? (
            <div role="alert" className="notice notice-error">
              <p>{t('network.busy')}</p>
              <button type="button" className="button-secondary" onClick={() => void openPending()}>
                {t('network.open_transaction')}
              </button>
            </div>
          ) : null}
          {failure ? <ErrorNotice error={failure} /> : null}
          {staging ? (
            <p role="status" className="muted">
              {t('network.staging')}
            </p>
          ) : null}
          <button type="submit" disabled={locked || staging}>
            {t('network.stage_submit')}
          </button>
        </form>
      )}
    </main>
  );
}
