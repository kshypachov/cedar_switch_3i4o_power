import { useEffect, useRef, useState } from 'react';

import { scheduler } from './scheduler';

export interface Polled<T> {
  data: T | null;
  error: unknown;
  loading: boolean;
}

/**
 * Keep @p load's result fresh on the shared scheduler. The request in flight is
 * aborted when the component goes away (plan section 4).
 */
export function usePolling<T>(
  load: (signal: AbortSignal) => Promise<T>,
  intervalMs: number,
  options: { stopOn?: (error: unknown) => boolean } = {},
): Polled<T> {
  const [state, setState] = useState<Polled<T>>({ data: null, error: null, loading: true });
  const loadRef = useRef(load);
  loadRef.current = load;
  const stopOnRef = useRef(options.stopOn);
  stopOnRef.current = options.stopOn;

  useEffect(() => {
    let alive = true;
    const stop = scheduler.every(intervalMs, async (signal) => {
      try {
        const data = await loadRef.current(signal);
        if (alive) setState({ data, error: null, loading: false });
      } catch (error) {
        if (alive && !signal.aborted) setState((prev) => ({ ...prev, error, loading: false }));
        // A resource this firmware does not serve will not appear by asking
        // again every few seconds.
        if (stopOnRef.current?.(error)) stop();
      }
    });
    return () => {
      alive = false;
      stop();
    };
  }, [intervalMs]);

  return state;
}
