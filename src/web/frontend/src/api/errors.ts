import type { components } from './schema.gen';

export type ErrorDetail = components['schemas']['ErrorDetail'];
export type ErrorField = components['schemas']['ErrorField'];

/**
 * Why a request did not produce its answer, as the interface needs to know it.
 *
 * - `api`: the device refused with the contract's error body; `detail` holds it.
 * - `busy`: a 409 with no JSON body. Zephyr's HTTP server sends that by itself,
 *   before any application code runs, when another client is in the middle of
 *   a request to the same resource (measured in P2). Nothing was executed, so
 *   repeating is safe for every method.
 * - `network`: no response at all.
 * - `timeout`: the client gave up waiting.
 * - `protocol`: a response that is neither a success nor the error body.
 */
export type ApiFailureKind = 'api' | 'busy' | 'network' | 'timeout' | 'protocol';

export class ApiFailure extends Error {
  readonly kind: ApiFailureKind;
  readonly status: number;
  readonly detail: ErrorDetail | null;
  readonly retryAfterSeconds: number | null;

  constructor(
    kind: ApiFailureKind,
    status: number,
    detail: ErrorDetail | null,
    retryAfterSeconds: number | null = null,
  ) {
    super(detail ? `${detail.code}: ${detail.message}` : `${kind} (${status})`);
    this.name = 'ApiFailure';
    this.kind = kind;
    this.status = status;
    this.detail = detail;
    this.retryAfterSeconds = retryAfterSeconds;
  }

  get code(): string | null {
    return this.detail?.code ?? null;
  }

  /** Field problems keyed by JSON Pointer, for placing next to inputs. */
  fieldCodes(): Map<string, string> {
    return new Map((this.detail?.fields ?? []).map((f) => [f.path, f.code]));
  }
}

export function isApiFailure(error: unknown, code?: string): error is ApiFailure {
  return error instanceof ApiFailure && (code === undefined || error.code === code);
}
