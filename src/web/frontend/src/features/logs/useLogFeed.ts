import { useEffect, useRef, useState } from 'react';

import { isApiFailure } from '../../api/errors';
import { getLogRecords } from '../../api/logs';
import { scheduler } from '../../state/scheduler';
import type { Polled } from '../../state/usePolling';
import { notServed } from '../../state/useSessionGuard';
import {
  CATCH_UP_PAGES,
  type FeedState,
  type Filters,
  applyPage,
  dropCursor,
  filtersKey,
  initialFeed,
  recordsQuery,
  withFilters,
} from './controller';

/** Live polling (contract: 500-1000 ms, the next request only after the previous). */
export const POLL_MS = 1_000;

export interface LogFeed {
  state: FeedState;
  /** For useSessionGuard and the error notice. */
  polled: Polled<FeedState>;
}

/**
 * The live tail on the shared scheduler. One task, restarted on a filter
 * change or a resume, stopped while paused - so a paused screen sends nothing,
 * and resuming continues from the cursor it had: whatever the ring overwrote
 * meanwhile arrives as the device's gap.
 */
export function useLogFeed(filters: Filters, paused: boolean): LogFeed {
  const [state, setState] = useState<FeedState>(() => initialFeed(filters));
  const [error, setError] = useState<unknown>(null);
  const [loading, setLoading] = useState(true);
  const stateRef = useRef(state);
  const key = filtersKey(filters);

  const update = (next: FeedState) => {
    stateRef.current = next;
    setState(next);
  };

  useEffect(() => {
    update(withFilters(stateRef.current, filters));
    if (paused) return undefined;
    let stopped = false;
    const stop = scheduler.every(POLL_MS, async (signal) => {
      for (let page = 0; page < CATCH_UP_PAGES && !stopped; page++) {
        const current = stateRef.current;
        let result;
        try {
          result = await getLogRecords(recordsQuery(current.filters, current.cursor), signal);
        } catch (failure) {
          if (signal.aborted) return;
          if (isApiFailure(failure, 'invalid_cursor') && current.cursor !== null) {
            update(dropCursor(current));
            continue;
          }
          setError(failure);
          setLoading(false);
          // A firmware without logs will not grow them by being asked every second.
          if (notServed(failure)) {
            stopped = true;
            stop();
          }
          return;
        }
        if (signal.aborted) return;
        update(applyPage(stateRef.current, result));
        setError(null);
        setLoading(false);
        if (!result.has_more) return;
      }
    });
    return () => {
      stopped = true;
      stop();
    };
    // `filters` is represented by its key; a new object with the same values changes nothing.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key, paused]);

  return { state, polled: { data: state, error, loading } };
}
