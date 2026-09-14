import { describe, expect, it } from 'vitest';

import { type ExportQuery, exportUrl } from '../../api/logs';
import type { LogPage } from '../../api/types';
import { logPage, logPageAfterReboot, logPageNext } from '../../test/fixtures';
import {
  DEFAULT_FILTERS,
  ROW_CAP,
  applyPage,
  dropCursor,
  exportQuery,
  filterQuery,
  formatUptime,
  initialFeed,
  isAtBottom,
  recordsQuery,
  withFilters,
} from './controller';

const records = (state: ReturnType<typeof initialFeed>) =>
  state.rows.filter((r) => r.type === 'record').map((r) => (r.type === 'record' ? r.record.seq : ''));

describe('queries', () => {
  it('leave defaults out, so the first request is a plain tail', () => {
    expect(recordsQuery(DEFAULT_FILTERS, null)).toEqual({ limit: 100 });
  });

  it('carry every filter and the cursor', () => {
    const filters = { source: 'esp32', minLevel: 'warning', module: ' wifi ', contains: 'AP not' } as const;
    expect(recordsQuery(filters, 'abc', 20)).toEqual({
      source: 'esp32',
      min_level: 'warning',
      module: 'wifi',
      contains: 'AP not',
      cursor: 'abc',
      limit: 20,
    });
  });

  it('keep spaces a search means and drop an empty module', () => {
    expect(filterQuery({ ...DEFAULT_FILTERS, module: '   ', contains: ' ' })).toEqual({ contains: ' ' });
  });

  it('build an export link with the same filters and no page parameters', () => {
    const url = exportUrl(exportQuery({ ...DEFAULT_FILTERS, source: 'stm32', contains: 'a&b' }, 'text'));
    expect(url.startsWith('/api/v1/logs/export?')).toBe(true);
    const params = new URL(url, 'http://device').searchParams;
    expect(Object.fromEntries(params)).toEqual({ source: 'stm32', contains: 'a&b', format: 'text' });
  });

  it('leave a parameter without a value out of the export link', () => {
    const query = { format: 'ndjson', source: undefined } as unknown as ExportQuery;
    expect(exportUrl(query)).toBe('/api/v1/logs/export?format=ndjson');
  });
});

describe('the feed', () => {
  it('appends pages in order and continues from the cursor it was given', () => {
    let state = applyPage(initialFeed(), logPage);
    expect(records(state)).toEqual(['1', '2', '3', '4', '5', '6']);
    expect(state.cursor).toBe(logPage.next_cursor);
    state = applyPage(state, logPageNext);
    expect(records(state)).toEqual(['1', '2', '3', '4', '5', '6', '7']);
    expect(state.cursor).toBe(logPageNext.next_cursor);
    expect(recordsQuery(state.filters, state.cursor).cursor).toBe(logPageNext.next_cursor);
  });

  it('keeps the cursor an empty page returns: the scan moved on', () => {
    const state = applyPage(applyPage(initialFeed(), logPage), { ...logPageNext, items: [], has_more: true, next_cursor: 'moved' });
    expect(state.cursor).toBe('moved');
    expect(records(state)).toHaveLength(6);
  });

  it('marks a gap where it happened, but not on a tail', () => {
    const tail = applyPage(initialFeed(), { ...logPage, gap: true });
    expect(tail.rows.some((r) => r.type === 'gap')).toBe(false);
    const next = applyPage(tail, { ...logPageNext, gap: true });
    expect(next.rows.map((r) => r.type).slice(-2)).toEqual(['gap', 'record']);
  });

  it('marks a new boot before its records, and not for the first page', () => {
    let state = applyPage(initialFeed(), logPage);
    expect(state.rows.some((r) => r.type === 'boot')).toBe(false);
    state = applyPage(state, logPageAfterReboot);
    const types = state.rows.map((r) => r.type).slice(-3);
    expect(types).toEqual(['boot', 'gap', 'record']);
    const boot = state.rows.find((r) => r.type === 'boot');
    expect(boot?.type === 'boot' && boot.bootId).toBe('boot_fedcba9876543210');
    expect(state.bootId).toBe('boot_fedcba9876543210');
  });

  it('gives every row a unique key: seq restarts with a boot, a gap record can share a seq', () => {
    let state = applyPage(initialFeed(), logPage);
    state = applyPage(state, logPageAfterReboot); // seq 1 again, other boot
    const gapRecord = { ...logPageNext.items[0]!, kind: 'gap' as const, seq: '1', boot_id: 'boot_fedcba9876543210' };
    state = applyPage(state, { ...logPageAfterReboot, gap: false, items: [gapRecord] });
    const keys = state.rows.map((r) => r.key);
    expect(new Set(keys).size).toBe(keys.length);
  });

  it('keeps a gap record and a message that share boot, source and seq', () => {
    const message = logPageNext.items[0]!;
    const gap = { ...message, kind: 'gap' as const };
    const state = applyPage(initialFeed(), { ...logPageNext, items: [message, gap] });
    expect(state.rows.filter((r) => r.type === 'record')).toHaveLength(2);
  });

  it('does not show a record twice if a page repeats it', () => {
    const state = applyPage(applyPage(initialFeed(), logPage), logPage);
    expect(records(state)).toEqual(['1', '2', '3', '4', '5', '6']);
  });

  it('keeps the newest rows within the cap and counts what it removed', () => {
    const many = (from: number, n: number): LogPage => ({
      ...logPageNext,
      items: Array.from({ length: n }, (_, i) => ({ ...logPageNext.items[0]!, seq: String(from + i) })),
    });
    let state = initialFeed();
    for (let page = 0; page < 25; page++) state = applyPage(state, many(page * 100 + 1, 100));
    expect(state.rows).toHaveLength(ROW_CAP);
    expect(state.trimmed).toBe(2500 - ROW_CAP);
    expect(records(state)[0]).toBe(String(2500 - ROW_CAP + 1));
    expect(records(state).at(-1)).toBe('2500');
  });

  it('starts over on a filter change and remembers the boot', () => {
    const state = applyPage(initialFeed(), logPage);
    const same = withFilters(state, { ...DEFAULT_FILTERS });
    expect(same).toBe(state);
    const changed = withFilters(state, { ...DEFAULT_FILTERS, source: 'esp32' });
    expect(changed.cursor).toBeNull();
    expect(changed.rows).toEqual([]);
    expect(changed.bootId).toBe(logPage.boot_id);
    expect(changed.filters.source).toBe('esp32');
  });

  it('drops the cursor and the rows after invalid_cursor', () => {
    const state = dropCursor(applyPage(initialFeed(), logPage));
    expect(state.cursor).toBeNull();
    expect(state.rows).toEqual([]);
    expect(recordsQuery(state.filters, state.cursor)).toEqual({ limit: 100 });
  });
});

describe('display helpers', () => {
  it('formats uptime without losing precision or wrapping days', () => {
    expect(formatUptime('4')).toBe('0:00:00.004');
    expect(formatUptime('3723456')).toBe('1:02:03.456');
    expect(formatUptime('93784000')).toBe('26:03:04.000');
    expect(formatUptime('18446744073709551615')).toBe('5124095576030:25:51.615');
  });

  it('follows only near the end of the list', () => {
    expect(isAtBottom(900, 100, 1000)).toBe(true);
    expect(isAtBottom(893, 100, 1000)).toBe(true);
    expect(isAtBottom(892, 100, 1000)).toBe(true);
    expect(isAtBottom(891, 100, 1000)).toBe(false);
    expect(isAtBottom(800, 100, 1000)).toBe(false);
    expect(isAtBottom(0, 0, 0)).toBe(true);
  });
});
