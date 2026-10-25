/*
 * Loads readings for one or more stations, then keeps them current with each station's live
 * subscription. When the query changes (another range), the previous data stays on screen,
 * marked as refreshing, until the new data arrives. reload() queries again, which also catches
 * up on anything missed while disconnected.
 */
import { useCallback, useEffect, useState } from 'react';
import { subscribeReadings } from './api';
import { mergeReadings, type Reading } from './readings';

export type LiveState =
  | { status: 'loading' }
  | { status: 'error'; message: string }
  | { status: 'ready'; readings: Reading[]; refreshing?: boolean; liveError?: string };

const message = (e: unknown) => (e instanceof Error ? e.message : String(e));

export function useLiveReadings(
  stationIds: readonly string[],
  queryKey: string, // changes when the query changes, e.g. the range
  load: () => Promise<Reading[]>,
  accept: (r: Reading) => boolean,
) {
  const [state, setState] = useState<LiveState>({ status: 'loading' });
  const [attempt, setAttempt] = useState(0);
  const reload = useCallback(() => setAttempt((n) => n + 1), []);

  useEffect(() => {
    let active = true;
    setState((s) => (s.status === 'ready' ? { ...s, refreshing: true } : { status: 'loading' }));
    load()
      .then((readings) => active && setState({ status: 'ready', readings: mergeReadings([], readings) }))
      .catch((e: unknown) => active && setState({ status: 'error', message: message(e) }));

    const unsubscribes = stationIds.map((id) =>
      subscribeReadings(
        id,
        (incoming) => {
          const wanted = incoming.filter(accept);
          if (!active || wanted.length === 0) return;
          setState((s) => (s.status === 'ready' ? { ...s, readings: mergeReadings(s.readings, wanted) } : s));
        },
        (e) => active && setState((s) => (s.status === 'ready' ? { ...s, liveError: e.message } : s)),
      ),
    );
    return () => {
      active = false;
      unsubscribes.forEach((unsubscribe) => unsubscribe());
    };
    // load and accept are rebuilt by callers each render; queryKey says when the query changed.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [stationIds.join(','), queryKey, attempt]);

  return { state, reload };
}

/** The current time, refreshed every second, for "updated 12 s ago" and online status. */
export function useNow(intervalMs = 1000): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), intervalMs);
    return () => clearInterval(id);
  }, [intervalMs]);
  return now;
}
