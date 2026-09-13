import createClient, { type Middleware } from 'openapi-fetch';

import { ApiFailure, type ErrorDetail } from './errors';
import type { paths } from './schema.gen';

/** The contract's base URL. Relative: the page and the API share one origin. */
export const API_BASE = '/api/v1';

const TIMEOUT_MS = 10_000;
const BUSY_RETRIES = 3;
const BUSY_BACKOFF_MS = 250;

/**
 * Requests one page may have in flight. The device serves four HTTP clients
 * and the plan budgets for two browsers; a page that fanned out one request
 * per card would take every slot by itself and lock the second browser out.
 */
export const MAX_IN_FLIGHT = 2;

let inFlight = 0;
const waiting: (() => void)[] = [];

async function acquire(): Promise<void> {
  if (inFlight < MAX_IN_FLIGHT) {
    inFlight++;
    return;
  }
  await new Promise<void>((resolve) => waiting.push(resolve));
}

function release(): void {
  const next = waiting.shift();
  if (next) {
    next();
  } else {
    inFlight--;
  }
}

/** For tests: how many requests hold a slot right now. */
export function requestsInFlight(): number {
  return inFlight;
}

let fetchImpl: typeof fetch = (...args) => fetch(...args);

/** Tests replace the transport; nothing else should. */
export function setFetch(impl: typeof fetch): void {
  fetchImpl = impl;
}

function sleep(ms: number, signal?: AbortSignal | null): Promise<void> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(resolve, ms);
    signal?.addEventListener(
      'abort',
      () => {
        clearTimeout(timer);
        reject(signal.reason);
      },
      { once: true },
    );
  });
}

/**
 * The transport under the typed client: a timeout, and a bounded retry of the
 * server's own bare 409 (see ApiFailure). Retrying is safe because nothing
 * ran; for a mutation it also carries the same Idempotency-Key, so even a
 * request that did run would be answered with its first result.
 */
async function transport(request: Request): Promise<Response> {
  await acquire();
  try {
    return await send(request);
  } finally {
    release();
  }
}

async function send(request: Request): Promise<Response> {
  for (let attempt = 0; ; attempt++) {
    const timeout = AbortSignal.timeout(TIMEOUT_MS);
    const signal = request.signal ? AbortSignal.any([request.signal, timeout]) : timeout;
    let response: Response;
    try {
      response = await fetchImpl(new Request(request.clone(), { signal }));
    } catch (error) {
      if (request.signal?.aborted) throw error;
      if (timeout.aborted) throw new ApiFailure('timeout', 0, null);
      throw new ApiFailure('network', 0, null);
    }
    const isBareConflict =
      response.status === 409 && !response.headers.get('content-type')?.includes('json');
    if (!isBareConflict || attempt >= BUSY_RETRIES) return response;
    await sleep(BUSY_BACKOFF_MS * (attempt + 1), request.signal);
  }
}

const refusals: Middleware = {
  async onResponse({ response }) {
    if (response.ok) return undefined;
    const retryAfter = Number.parseInt(response.headers.get('retry-after') ?? '', 10);
    const retry = Number.isFinite(retryAfter) ? retryAfter : null;
    const type = response.headers.get('content-type') ?? '';
    if (response.status === 409 && !type.includes('json')) {
      throw new ApiFailure('busy', 409, null, retry);
    }
    if (type.includes('application/json')) {
      const body = (await response.clone().json().catch(() => null)) as {
        error?: ErrorDetail;
      } | null;
      if (body?.error && typeof body.error.code === 'string') {
        throw new ApiFailure('api', response.status, body.error, retry);
      }
    }
    throw new ApiFailure('protocol', response.status, null, retry);
  },
};

// Absolute but same-origin: the page's own origin plus the base path. A bare
// relative base works in a browser, not in every Request implementation.
const origin = typeof window === 'undefined' ? '' : window.location.origin;

export const api = createClient<paths>({
  baseUrl: `${origin}${API_BASE}`,
  fetch: transport,
  credentials: 'same-origin',
});
api.use(refusals);

/** A fresh Idempotency-Key: 32 URL-safe characters (the contract allows 16-64). */
export function newIdempotencyKey(): string {
  const bytes = new Uint8Array(24);
  crypto.getRandomValues(bytes);
  return btoa(String.fromCharCode(...bytes))
    .replace(/\+/g, '-')
    .replace(/\//g, '_')
    .replace(/=+$/, '');
}

/**
 * The value of a successful call, or the failure thrown by the middleware.
 * openapi-fetch returns `{data, error}`; with the middleware every refusal has
 * already thrown, so an `error` here means a 2xx the schema did not expect.
 */
export async function unwrap<T>(
  call: Promise<{ data?: T; error?: unknown; response: Response }>,
): Promise<T> {
  const { data, error, response } = await call;
  if (error !== undefined) throw new ApiFailure('protocol', response.status, null);
  return data as T;
}
