import { API_BASE, api, unwrap } from './client';
import type { paths } from './schema.gen';
import type { LogPage, LogSources } from './types';

/** The query of GET /logs/records, as the document declares it. */
export type RecordsQuery = NonNullable<paths['/logs/records']['get']['parameters']['query']>;

/** The query of GET /logs/export. */
export type ExportQuery = NonNullable<paths['/logs/export']['get']['parameters']['query']>;

export const getLogRecords = (query: RecordsQuery, signal?: AbortSignal): Promise<LogPage> =>
  unwrap(api.GET('/logs/records', { params: { query }, signal }));

export const getLogSources = (signal?: AbortSignal): Promise<LogSources> =>
  unwrap(api.GET('/logs/sources', { signal }));

/**
 * The export is a download, not a request the page reads: a plain same-origin
 * link, so the browser saves the attachment under the name the device gives in
 * Content-Disposition and the session cookie goes with it.
 */
export function exportUrl(query: ExportQuery): string {
  const params = new URLSearchParams();
  for (const [name, value] of Object.entries(query)) {
    if (value !== undefined) params.set(name, String(value));
  }
  return `${API_BASE}/logs/export?${params.toString()}`;
}
