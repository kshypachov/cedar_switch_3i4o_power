import { useEffect, useRef, useState } from 'react';

import { getCoprocessorStatus } from '../../api/device';
import { exportUrl, getLogSources } from '../../api/logs';
import type { LogSource } from '../../api/types';
import { ErrorNotice } from '../../components/ErrorNotice';
import { Body } from '../../components/Polled';
import { Card, Facts, Loading } from '../../components/ui';
import { t, tMaybe } from '../../i18n';
import { usePolling } from '../../state/usePolling';
import { notServed, useSessionGuard } from '../../state/useSessionGuard';
import {
  DEFAULT_FILTERS,
  type Filters,
  type LevelFilter,
  ROW_CAP,
  type Row,
  type SourceFilter,
  exportQuery,
  formatUptime,
  isAtBottom,
} from './controller';
import { useLogFeed } from './useLogFeed';

const SOURCES_MS = 5_000;
const TEXT_DEBOUNCE_MS = 400;
const LEVELS: Exclude<LevelFilter, ''>[] = ['debug', 'info', 'warning', 'error'];

function reasonText(reason: string | null): string {
  if (!reason) return t('value.unknown');
  return tMaybe(`logs.reason.${reason}`) ?? reason;
}

function RowView({ row }: { row: Row }) {
  if (row.type === 'boot') {
    return (
      <tr className="log-boot" data-testid="log-boot">
        <td colSpan={5}>{t('logs.boot_changed', { id: row.bootId })}</td>
      </tr>
    );
  }
  if (row.type === 'gap') {
    return (
      <tr className="log-gap" data-testid="log-gap">
        <td colSpan={5}>{t('logs.gap')}</td>
      </tr>
    );
  }
  const r = row.record;
  return (
    <tr className={`log-row log-${r.kind} log-level-${r.level ?? 'none'}`} data-testid="log-row" data-source={r.source}>
      <td className="log-time">{formatUptime(r.uptime_ms)}</td>
      <td>{t(`logs.source.${r.source}`)}</td>
      <td>{r.level === null ? t('value.none') : t(`logs.level.${r.level}`)}</td>
      <td>{r.module ?? t('value.none')}</td>
      <td className="log-message">
        {r.kind !== 'message' ? <span className="log-kind">{t(`logs.kind.${r.kind}`)}</span> : null}
        {/* A log line is text: React escapes it, nothing is ever parsed as markup. */}
        <span className="log-text">{r.message}</span>
        {r.truncated ? <span className="log-truncated">{t('logs.truncated')}</span> : null}
      </td>
    </tr>
  );
}

function SourcesTable({ items }: { items: LogSource[] }) {
  return (
    <div className="table-scroll">
      <table>
        <thead>
          <tr>
            <th scope="col">{t('logs.col_source')}</th>
            <th scope="col">{t('logs.col_state')}</th>
            <th scope="col">{t('logs.col_generation')}</th>
            <th scope="col">{t('logs.col_dropped')}</th>
          </tr>
        </thead>
        <tbody>
          {items.map((s) => (
            <tr key={s.id} data-testid={`log-source-${s.id}`}>
              <td>{t(`logs.source.${s.id}`)}</td>
              <td>{s.available ? t('logs.available') : t('logs.unavailable', { reason: reasonText(s.reason) })}</td>
              <td>{s.generation}</td>
              <td>{s.dropped_count}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export function LogsScreen() {
  const [filters, setFilters] = useState<Filters>(DEFAULT_FILTERS);
  const [moduleDraft, setModuleDraft] = useState('');
  const [containsDraft, setContainsDraft] = useState('');
  const [paused, setPaused] = useState(false);
  const [following, setFollowing] = useState(true);

  // Text filters settle before they restart the tail: one request per pause in typing.
  useEffect(() => {
    const timer = setTimeout(() => {
      setFilters((f) =>
        f.module === moduleDraft.trim() && f.contains === containsDraft
          ? f
          : { ...f, module: moduleDraft.trim(), contains: containsDraft },
      );
    }, TEXT_DEBOUNCE_MS);
    return () => clearTimeout(timer);
  }, [moduleDraft, containsDraft]);

  const feed = useLogFeed(filters, paused);
  const sources = usePolling(getLogSources, SOURCES_MS, { stopOn: notServed });
  const coprocessor = usePolling(getCoprocessorStatus, SOURCES_MS, { stopOn: notServed });
  useSessionGuard(feed.polled, sources, coprocessor);

  const box = useRef<HTMLDivElement>(null);
  const rows = feed.state.rows;
  useEffect(() => {
    const el = box.current;
    if (el && following && !paused) el.scrollTop = el.scrollHeight;
  }, [rows, following, paused]);

  const error = feed.polled.error;

  return (
    <main>
      <h1>{t('logs.title')}</h1>
      <Card title="logs.filters">
        <div className="field-row">
          <div className="field">
            <label htmlFor="logs-source">{t('logs.source_label')}</label>
            <select
              id="logs-source"
              value={filters.source}
              onChange={(e) => setFilters((f) => ({ ...f, source: e.target.value as SourceFilter }))}
            >
              <option value="all">{t('logs.source_all')}</option>
              <option value="stm32">{t('logs.source.stm32')}</option>
              <option value="esp32">{t('logs.source.esp32')}</option>
            </select>
          </div>
          <div className="field">
            <label htmlFor="logs-level">{t('logs.level_label')}</label>
            <select
              id="logs-level"
              value={filters.minLevel}
              onChange={(e) => setFilters((f) => ({ ...f, minLevel: e.target.value as LevelFilter }))}
            >
              <option value="">{t('logs.level_any')}</option>
              {LEVELS.map((level) => (
                <option key={level} value={level}>
                  {t(`logs.level.${level}`)}
                </option>
              ))}
            </select>
          </div>
          <div className="field">
            <label htmlFor="logs-module">{t('logs.module_label')}</label>
            <input id="logs-module" value={moduleDraft} maxLength={64} onChange={(e) => setModuleDraft(e.target.value)} />
          </div>
          <div className="field">
            <label htmlFor="logs-contains">{t('logs.contains_label')}</label>
            <input
              id="logs-contains"
              value={containsDraft}
              maxLength={128}
              onChange={(e) => setContainsDraft(e.target.value)}
            />
          </div>
        </div>
        <p className="muted field-hint">{t('logs.filter_hint')}</p>
      </Card>

      <Card title="logs.records">
        <div className="actions">
          <button type="button" className="button-secondary" onClick={() => setPaused((p) => !p)}>
            {t(paused ? 'logs.resume' : 'logs.pause')}
          </button>
          {!following && !paused ? (
            <button type="button" className="button-secondary" onClick={() => setFollowing(true)}>
              {t('logs.follow')}
            </button>
          ) : null}
        </div>
        {paused ? (
          <p role="status" className="notice">
            {t('logs.paused_notice')}
          </p>
        ) : null}
        {feed.state.bootId ? (
          <p className="muted">
            {t('logs.boot_id')}: <code data-testid="log-boot-id">{feed.state.bootId}</code>
          </p>
        ) : null}
        {feed.state.trimmed > 0 ? <p className="muted">{t('logs.row_cap', { n: ROW_CAP })}</p> : null}
        {error && notServed(error) ? (
          <p className="muted">{t('overview.unavailable')}</p>
        ) : error ? (
          <ErrorNotice error={error} />
        ) : null}
        <div
          className="log-scroll"
          ref={box}
          role="log"
          aria-label={t('logs.records')}
          tabIndex={0}
          onScroll={() => {
            const el = box.current;
            if (el) setFollowing(isAtBottom(el.scrollTop, el.clientHeight, el.scrollHeight));
          }}
        >
          {rows.length === 0 ? (
            feed.polled.loading ? (
              <Loading />
            ) : (
              <p className="muted">{t('logs.empty')}</p>
            )
          ) : (
            <table className="logs">
              <thead>
                <tr>
                  <th scope="col">{t('logs.col_time')}</th>
                  <th scope="col">{t('logs.col_source')}</th>
                  <th scope="col">{t('logs.col_level')}</th>
                  <th scope="col">{t('logs.col_module')}</th>
                  <th scope="col">{t('logs.col_message')}</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((row) => (
                  <RowView key={row.key} row={row} />
                ))}
              </tbody>
            </table>
          )}
        </div>
      </Card>

      <Card title="logs.export">
        <p>{t('logs.export_intro')}</p>
        <div className="actions">
          <a className="button-link" href={exportUrl(exportQuery(filters, 'ndjson'))} download data-testid="log-export-ndjson">
            {t('logs.export_ndjson')}
          </a>
          <a className="button-link" href={exportUrl(exportQuery(filters, 'text'))} download data-testid="log-export-text">
            {t('logs.export_text')}
          </a>
        </div>
      </Card>

      <Card title="logs.sources">
        <Body polled={sources} render={(s) => <SourcesTable items={s.items} />} />
        {coprocessor.data ? (
          <Facts rows={[['logs.uart_mode', t(`logs.uart_mode.${coprocessor.data.uart_mode}`)]]} />
        ) : null}
        <p className="muted field-hint">{t('logs.dropped_hint')}</p>
      </Card>
    </main>
  );
}
