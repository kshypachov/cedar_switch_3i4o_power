import { vi } from 'vitest';

import { setFetch } from '../api/client';

export type Handler = (request: Request, url: URL) => Response | Promise<Response>;

export function json(status: number, body: unknown, headers: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json', ...headers },
  });
}

export function refusal(status: number, code: string, extra: Record<string, unknown> = {}): Response {
  return json(status, {
    error: { code, message: code, request_id: 'req_00000001', retryable: false, ...extra },
  });
}

/**
 * A fake device: "METHOD /path" -> handler. Unmatched requests fail the test
 * loudly instead of hanging, and every request is kept for assertions.
 */
export function fakeDevice(routes: Record<string, Handler>) {
  const requests: Request[] = [];
  const impl = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
    const request = input instanceof Request ? input : new Request(input, init);
    requests.push(request.clone());
    const url = new URL(request.url, 'http://device.local');
    const key = `${request.method} ${url.pathname}`;
    const handler = routes[key];
    if (!handler) throw new Error(`unexpected request ${key}`);
    return handler(request, url);
  });
  setFetch(impl as unknown as typeof fetch);
  return { requests, fetch: impl };
}
