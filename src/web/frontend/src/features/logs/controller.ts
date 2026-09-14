import type { ExportQuery, RecordsQuery } from '../../api/logs';
import type { LogPage, LogRecord } from '../../api/types';

/**
 * The log screen's state, as pure functions of pages the device returns.
 *
 * The contract's paging is a live tail (api-contract.md, "Логи"): without a
 * cursor the device returns the newest records, and the cursor it gives back
 * is where to continue scanning - even when nothing matched. What this module
 * decides:
 *
 * - **A filter change starts over.** The contract binds a cursor to the
 *   filters that produced it and tells the client to drop it, so the rows of
 *   the old filter go too and the next request is a tail without a cursor.
 * - **A refused cursor starts over the same way.** `400 invalid_cursor` is not
 *   an error to show; the rows are dropped rather than risk showing a record
 *   twice when the tail overlaps them.
 * - **A gap and a new boot are rows**, placed where they happened, because
 *   "records were lost here" means nothing in a banner above two thousand
 *   lines. A gap on a tail request is ignored: there was no position to lose
 *   records after.
 * - **Keys never rest on seq alone.** Sequence numbers restart with a boot, and
 *   an export's gap record may share one with its neighbour.
 * - **The page keeps a bounded number of rows**; the device's ring is bounded
 *   too, and the export is how to get more.
 */

export type SourceFilter = 'all' | 'stm32' | 'esp32';
export type LevelFilter = '' | 'debug' | 'info' | 'warning' | 'error';

export interface Filters {
  source: SourceFilter;
  minLevel: LevelFilter;
  module: string;
  contains: string;
}

export const DEFAULT_FILTERS: Filters = { source: 'all', minLevel: '', module: '', contains: '' };

/** The contract's largest page; the device may return fewer (byte bound). */
export const PAGE_LIMIT = 100;
/** Rows kept on the page. */
export const ROW_CAP = 2000;
/** Pages fetched back to back in one tick while the device says has_more. */
export const CATCH_UP_PAGES = 5;

export type Row =
  | { type: 'record'; key: string; record: LogRecord }
  | { type: 'gap'; key: string }
  | { type: 'boot'; key: string; bootId: string };

export interface FeedState {
  filters: Filters;
  cursor: string | null;
  /** The boot the last page came from; survives a filter change. */
  bootId: string | null;
  rows: Row[];
  /** Rows removed from the top by ROW_CAP since the last start. */
  trimmed: number;
  /** For keys of rows that are not records. */
  serial: number;
}

export function filtersKey(filters: Filters): string {
  return JSON.stringify([filters.source, filters.minLevel, filters.module, filters.contains]);
}

/** The filter part of a query; defaults are left out, as the mock and device read them. */
export function filterQuery(filters: Filters): Pick<RecordsQuery, 'source' | 'min_level' | 'module' | 'contains'> {
  const query: Pick<RecordsQuery, 'source' | 'min_level' | 'module' | 'contains'> = {};
  if (filters.source !== 'all') query.source = filters.source;
  if (filters.minLevel !== '') query.min_level = filters.minLevel;
  const module = filters.module.trim();
  if (module !== '') query.module = module;
  if (filters.contains !== '') query.contains = filters.contains;
  return query;
}

export function recordsQuery(filters: Filters, cursor: string | null, limit = PAGE_LIMIT): RecordsQuery {
  return { ...filterQuery(filters), ...(cursor !== null ? { cursor } : {}), limit };
}

export function exportQuery(filters: Filters, format: 'ndjson' | 'text'): ExportQuery {
  return { ...filterQuery(filters), format };
}

export function initialFeed(filters: Filters = DEFAULT_FILTERS): FeedState {
  return { filters, cursor: null, bootId: null, rows: [], trimmed: 0, serial: 0 };
}

export function withFilters(state: FeedState, filters: Filters): FeedState {
  if (filtersKey(filters) === filtersKey(state.filters)) return state;
  return { ...initialFeed(filters), bootId: state.bootId, serial: state.serial };
}

/** After `400 invalid_cursor`: the next request is a tail. */
export function dropCursor(state: FeedState): FeedState {
  return { ...state, cursor: null, rows: [], trimmed: 0 };
}

export function recordKey(record: LogRecord): string {
  return `${record.boot_id}:${record.source}:${record.kind}:${record.seq}`;
}

export function applyPage(state: FeedState, page: LogPage): FeedState {
  const rows = [...state.rows];
  let serial = state.serial;
  if (state.bootId !== null && page.boot_id !== state.bootId) {
    rows.push({ type: 'boot', key: `boot:${++serial}`, bootId: page.boot_id });
  }
  if (page.gap && state.cursor !== null) {
    rows.push({ type: 'gap', key: `gap:${++serial}` });
  }
  const seen = new Set(rows.filter((r) => r.type === 'record').map((r) => r.key));
  for (const record of page.items) {
    const key = recordKey(record);
    if (seen.has(key)) continue;
    seen.add(key);
    rows.push({ type: 'record', key, record });
  }
  const excess = Math.max(0, rows.length - ROW_CAP);
  return {
    ...state,
    rows: excess > 0 ? rows.slice(excess) : rows,
    trimmed: state.trimmed + excess,
    cursor: page.next_cursor,
    bootId: page.boot_id,
    serial,
  };
}

/** Within a few pixels of the end: follow new rows. Scrolled up: stop following. */
export function isAtBottom(scrollTop: number, clientHeight: number, scrollHeight: number, slack = 8): boolean {
  return scrollHeight - (scrollTop + clientHeight) <= slack;
}

/** uptime_ms (a decimal string) as h:mm:ss.mmm; hours are not wrapped at a day. */
export function formatUptime(uptimeMs: string): string {
  const total = BigInt(uptimeMs);
  const ms = Number(total % 1000n);
  const seconds = total / 1000n;
  const s = Number(seconds % 60n);
  const m = Number((seconds / 60n) % 60n);
  const h = seconds / 3600n;
  const two = (n: number) => String(n).padStart(2, '0');
  return `${h}:${two(m)}:${two(s)}.${String(ms).padStart(3, '0')}`;
}
