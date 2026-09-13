import { afterEach, describe, expect, it, vi } from 'vitest';

import { changePassword, login, setup } from './auth';
import { MAX_IN_FLIGHT, newIdempotencyKey, requestsInFlight } from './client';
import { getSystemStatus } from './device';
import { ApiFailure } from './errors';
import { fakeDevice, json, refusal } from '../test/server';
import { session, systemStatus } from '../test/fixtures';

afterEach(() => {
  vi.useRealTimers();
});

describe('refusals', () => {
  it('turns the contract error body into an ApiFailure with its code and Retry-After', async () => {
    fakeDevice({
      'POST /api/v1/auth/session': () =>
        json(429, { error: { code: 'rate_limited', message: 'x', request_id: 'req_1', retryable: true } }, { 'Retry-After': '4' }),
    });
    const failure = await login('pw').catch((e: unknown) => e);
    expect(failure).toBeInstanceOf(ApiFailure);
    expect((failure as ApiFailure).kind).toBe('api');
    expect((failure as ApiFailure).code).toBe('rate_limited');
    expect((failure as ApiFailure).status).toBe(429);
    expect((failure as ApiFailure).retryAfterSeconds).toBe(4);
  });

  it('keeps field errors addressable by pointer', async () => {
    fakeDevice({
      'POST /api/v1/auth/setup': () =>
        refusal(422, 'validation_failed', { fields: [{ path: '/password', code: 'out_of_range' }] }),
    });
    const failure = (await setup('token-token-token', 'short').catch((e: unknown) => e)) as ApiFailure;
    expect(failure.fieldCodes().get('/password')).toBe('out_of_range');
  });

  it('calls a response that is neither a success nor the error body a protocol failure', async () => {
    fakeDevice({ 'GET /api/v1/system/status': () => new Response('Internal Server Error', { status: 500 }) });
    const failure = (await getSystemStatus().catch((e: unknown) => e)) as ApiFailure;
    expect(failure.kind).toBe('protocol');
    expect(failure.status).toBe(500);
  });

  it('calls a failed fetch a network failure', async () => {
    const device = fakeDevice({});
    device.fetch.mockRejectedValue(new TypeError('Failed to fetch'));
    const failure = (await getSystemStatus().catch((e: unknown) => e)) as ApiFailure;
    expect(failure.kind).toBe('network');
  });
});

describe("the server's bare 409", () => {
  it('is retried, and the retry that succeeds is the answer', async () => {
    vi.useFakeTimers();
    let calls = 0;
    fakeDevice({
      'GET /api/v1/system/status': () => (++calls < 3 ? new Response('', { status: 409 }) : json(200, systemStatus)),
    });
    const pending = getSystemStatus();
    await vi.runAllTimersAsync();
    await expect(pending).resolves.toEqual(systemStatus);
    expect(calls).toBe(3);
  });

  it('becomes a busy failure when it persists', async () => {
    vi.useFakeTimers();
    let calls = 0;
    fakeDevice({
      'GET /api/v1/system/status': () => {
        calls++;
        return new Response('', { status: 409 });
      },
    });
    const pending = getSystemStatus().catch((e: unknown) => e);
    await vi.runAllTimersAsync();
    const failure = (await pending) as ApiFailure;
    expect(failure.kind).toBe('busy');
    expect(calls).toBe(4);
  });

  it('is not confused with a JSON 409 from the contract', async () => {
    let calls = 0;
    fakeDevice({
      'PUT /api/v1/auth/password': () => {
        calls++;
        return refusal(409, 'idempotency_conflict');
      },
    });
    const failure = (await changePassword('c', 'k'.repeat(20), 'a', 'b').catch((e: unknown) => e)) as ApiFailure;
    expect(failure.code).toBe('idempotency_conflict');
    expect(calls).toBe(1);
  });

  it('retries a mutation with the same Idempotency-Key', async () => {
    vi.useFakeTimers();
    const keys: string[] = [];
    fakeDevice({
      'PUT /api/v1/auth/password': (request) => {
        keys.push(request.headers.get('Idempotency-Key') ?? '');
        return keys.length === 1
          ? new Response('', { status: 409 })
          : json(202, { job_id: 'job_1', job_url: '/api/v1/jobs/job_1', resource_url: null });
      },
    });
    const pending = changePassword('csrf', 'the-one-key-for-this-change', 'old password!', 'new password!');
    await vi.runAllTimersAsync();
    await pending;
    expect(keys).toEqual(['the-one-key-for-this-change', 'the-one-key-for-this-change']);
  });
});

describe('headers', () => {
  it('sends what each operation declares, and nothing it does not', async () => {
    const device = fakeDevice({
      'POST /api/v1/auth/session': () => json(200, session),
      'POST /api/v1/auth/setup': () => json(201, session),
      'PUT /api/v1/auth/password': () => json(202, { job_id: 'job_1', job_url: '/api/v1/jobs/job_1', resource_url: null }),
    });
    await login('pw');
    await setup('the-setup-token-1', 'a good long password');
    await changePassword('csrf-token', 'idempotency-key-0001', 'a', 'b');
    const [loginReq, setupReq, changeReq] = device.requests;
    expect(loginReq!.headers.get('X-CSRF-Token')).toBeNull();
    expect(loginReq!.headers.get('Idempotency-Key')).toBeNull();
    expect(loginReq!.headers.get('Content-Type')).toContain('application/json');
    expect(await loginReq!.json()).toEqual({ password: 'pw' });
    expect(setupReq!.headers.get('X-Setup-Token')).toBe('the-setup-token-1');
    expect(changeReq!.headers.get('X-CSRF-Token')).toBe('csrf-token');
    expect(changeReq!.headers.get('Idempotency-Key')).toBe('idempotency-key-0001');
    expect(new URL(changeReq!.url).pathname).toBe('/api/v1/auth/password');
  });

  it('makes idempotency keys the contract accepts', () => {
    const keys = new Set(Array.from({ length: 50 }, () => newIdempotencyKey()));
    expect(keys.size).toBe(50);
    for (const key of keys) expect(key).toMatch(/^[A-Za-z0-9_-]{16,64}$/);
  });
});

describe('the page takes at most two client slots', () => {
  it('queues the third request until one finishes', async () => {
    const resolvers: ((r: Response) => void)[] = [];
    const device = fakeDevice({
      'GET /api/v1/system/status': () => new Promise<Response>((resolve) => resolvers.push(resolve)),
    });
    const pending = [getSystemStatus(), getSystemStatus(), getSystemStatus()];
    await vi.waitFor(() => expect(device.fetch).toHaveBeenCalledTimes(MAX_IN_FLIGHT));
    expect(requestsInFlight()).toBe(MAX_IN_FLIGHT);
    resolvers[0]!(json(200, systemStatus));
    await vi.waitFor(() => expect(device.fetch).toHaveBeenCalledTimes(3));
    resolvers[1]!(json(200, systemStatus));
    resolvers[2]!(json(200, systemStatus));
    await Promise.all(pending);
    expect(requestsInFlight()).toBe(0);
  });
});
