/**
 * The one polling scheduler (plan section 4: bounded polling, cancelled when
 * the view goes away, slower in a background tab).
 *
 * Each task runs at its interval, never overlapping itself: the next run is
 * scheduled only when the previous one has settled, so a slow device is not
 * sent a queue of requests - which on a server with four client slots is how
 * P0 kept it busy for three minutes. In a hidden tab every interval stretches
 * to BACKGROUND_MS, and the moment the tab is visible again each task runs.
 */

export const BACKGROUND_MS = 5_000;

export type Task = (signal: AbortSignal) => Promise<unknown>;

interface Entry {
  task: Task;
  intervalMs: number;
  timer: ReturnType<typeof setTimeout> | null;
  running: boolean;
  controller: AbortController;
}

export class Scheduler {
  private readonly entries = new Set<Entry>();
  private readonly doc: Document | null;

  constructor(doc: Document | null = typeof document === 'undefined' ? null : document) {
    this.doc = doc;
    this.doc?.addEventListener('visibilitychange', this.onVisibility);
  }

  /** Start @p task now and every @p intervalMs; returns a stop function. */
  every(intervalMs: number, task: Task): () => void {
    const entry: Entry = {
      task,
      intervalMs,
      timer: null,
      running: false,
      controller: new AbortController(),
    };
    this.entries.add(entry);
    void this.run(entry);
    return () => this.stop(entry);
  }

  dispose(): void {
    for (const entry of [...this.entries]) this.stop(entry);
    this.doc?.removeEventListener('visibilitychange', this.onVisibility);
  }

  private hidden(): boolean {
    return this.doc?.visibilityState === 'hidden';
  }

  private async run(entry: Entry): Promise<void> {
    if (!this.entries.has(entry) || entry.running) return;
    entry.running = true;
    try {
      await entry.task(entry.controller.signal);
    } catch {
      // The task reports its own failures; the schedule continues.
    } finally {
      entry.running = false;
    }
    this.schedule(entry);
  }

  private schedule(entry: Entry): void {
    if (!this.entries.has(entry)) return;
    const delay = this.hidden() ? Math.max(entry.intervalMs, BACKGROUND_MS) : entry.intervalMs;
    entry.timer = setTimeout(() => {
      entry.timer = null;
      void this.run(entry);
    }, delay);
  }

  private stop(entry: Entry): void {
    if (entry.timer) clearTimeout(entry.timer);
    entry.controller.abort();
    this.entries.delete(entry);
  }

  private readonly onVisibility = (): void => {
    if (this.hidden()) return;
    for (const entry of this.entries) {
      if (entry.running) continue;
      if (entry.timer) clearTimeout(entry.timer);
      entry.timer = null;
      void this.run(entry);
    }
  };
}

export const scheduler = new Scheduler();
