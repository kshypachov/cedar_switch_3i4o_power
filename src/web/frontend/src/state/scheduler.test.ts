import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { BACKGROUND_MS, Scheduler } from './scheduler';

function fakeDocument() {
  const listeners = new Set<() => void>();
  const doc = {
    visibilityState: 'visible' as DocumentVisibilityState,
    addEventListener: (_: string, fn: () => void) => listeners.add(fn),
    removeEventListener: (_: string, fn: () => void) => listeners.delete(fn),
    set(state: DocumentVisibilityState) {
      doc.visibilityState = state;
      listeners.forEach((fn) => fn());
    },
  };
  return doc;
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe('Scheduler', () => {
  it('runs at once and then every interval', async () => {
    const s = new Scheduler(null);
    const task = vi.fn().mockResolvedValue(undefined);
    s.every(1000, task);
    await vi.advanceTimersByTimeAsync(0);
    expect(task).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(1000);
    expect(task).toHaveBeenCalledTimes(2);
    await vi.advanceTimersByTimeAsync(3000);
    expect(task).toHaveBeenCalledTimes(5);
    s.dispose();
  });

  it('never overlaps a slow task with itself', async () => {
    const s = new Scheduler(null);
    let finish: () => void = () => {};
    const task = vi.fn(() => new Promise<void>((resolve) => (finish = resolve)));
    s.every(100, task);
    await vi.advanceTimersByTimeAsync(1000);
    expect(task).toHaveBeenCalledTimes(1);
    finish();
    await vi.advanceTimersByTimeAsync(100);
    expect(task).toHaveBeenCalledTimes(2);
    s.dispose();
  });

  it('keeps going after a failure', async () => {
    const s = new Scheduler(null);
    const task = vi.fn().mockRejectedValueOnce(new Error('x')).mockResolvedValue(undefined);
    s.every(100, task);
    await vi.advanceTimersByTimeAsync(250);
    expect(task).toHaveBeenCalledTimes(3);
    s.dispose();
  });

  it('slows down in a hidden tab and catches up when it is shown', async () => {
    const doc = fakeDocument();
    const s = new Scheduler(doc as unknown as Document);
    const task = vi.fn().mockResolvedValue(undefined);
    s.every(500, task);
    await vi.advanceTimersByTimeAsync(0);
    doc.set('hidden');
    await vi.advanceTimersByTimeAsync(500);
    const afterFirstInterval = task.mock.calls.length;
    await vi.advanceTimersByTimeAsync(BACKGROUND_MS - 1);
    expect(task.mock.calls.length).toBe(afterFirstInterval);
    doc.set('visible');
    await vi.advanceTimersByTimeAsync(0);
    expect(task.mock.calls.length).toBe(afterFirstInterval + 1);
    s.dispose();
  });

  it('stops and aborts the request in flight', async () => {
    const s = new Scheduler(null);
    let signal: AbortSignal | undefined;
    const stop = s.every(100, (sig) => {
      signal = sig;
      return new Promise(() => {});
    });
    await vi.advanceTimersByTimeAsync(0);
    stop();
    expect(signal?.aborted).toBe(true);
  });
});
