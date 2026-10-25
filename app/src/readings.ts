/*
 * Readings as the dashboard uses them: types from the API schema, and the small calculations the
 * pages share (time, delay, freshness, formatting). No React, no network.
 */
import type { Schema } from '../amplify/data/resource';

export type Reading = Schema['Reading']['type'];

/*
 * A station sends one minute summary a minute, so two minutes without anything means it is
 * offline. An assumption from the design; field trials would tune it.
 */
export const OFFLINE_AFTER_MS = 2 * 60_000;

/** When the reading was taken: a summary's minute start, or a change's sample time. Unix ms. */
export const takenAtMs = (r: Reading): number => r.ts * 1000 + r.tenths * 100;

/** Station to cloud: a summary is complete at the end of its minute, a change at its sample. */
export const delayMs = (r: Reading): number =>
  r.receivedAt - (r.kind === 'minute' ? (r.ts + 60) * 1000 : takenAtMs(r));

export const isOnline = (latest: Reading | undefined, nowMs: number): boolean =>
  latest !== undefined && nowMs - latest.receivedAt <= OFFLINE_AFTER_MS;

/* A reading's identity: the table key. sk alone repeats across stations. */
const key = (r: Reading) => `${r.stationId}|${r.sk}`;

/** Adds new readings, replacing any with the same key (a redelivery), oldest first. */
export function mergeReadings(existing: readonly Reading[], incoming: readonly Reading[]): Reading[] {
  const byKey = new Map(existing.map((r) => [key(r), r]));
  for (const r of incoming) byKey.set(key(r), r);
  return [...byKey.values()].sort((a, b) => takenAtMs(a) - takenAtMs(b) || key(a).localeCompare(key(b)));
}

/** Readings grouped by station, in the given station order (every station present, maybe empty). */
export function byStation(stationIds: readonly string[], readings: readonly Reading[]): Map<string, Reading[]> {
  const groups = new Map(stationIds.map((id) => [id, [] as Reading[]]));
  for (const r of readings) groups.get(r.stationId)?.push(r);
  return groups;
}

/** The most recently received reading, if any. */
export const newest = (readings: readonly Reading[]): Reading | undefined =>
  readings.reduce<Reading | undefined>((best, r) => (!best || r.receivedAt > best.receivedAt ? r : best), undefined);

export function ageText(ms: number): string {
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 5) return 'just now';
  if (s < 60) return `${s} s ago`;
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`;
  return `${Math.floor(s / 86400)} d ago`;
}

const one = (v: number | null | undefined, unit: string) =>
  v === null || v === undefined ? 'n/a' : `${v.toFixed(1)} ${unit}`;

export const formatTemp = (celsius?: number | null) => one(celsius, '°C');
export const formatRh = (percent?: number | null) => one(percent, '%');

const timeFormat = new Intl.DateTimeFormat('en-GB', {
  day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit', second: '2-digit',
});

export const formatTime = (ms: number): string => timeFormat.format(ms);

export interface RangeStats {
  latest?: Reading;
  tempMin?: number;
  tempMax?: number;
  rhMin?: number;
  rhMax?: number;
}

/** One station over the range: its newest reading and the extremes of temperature and humidity. */
export function rangeStats(readings: readonly Reading[]): RangeStats {
  const pick = (f: (r: Reading) => number | null | undefined) =>
    readings.map(f).filter((v): v is number => v !== null && v !== undefined);
  const lows = (f: (r: Reading) => number | null | undefined) => (pick(f).length ? Math.min(...pick(f)) : undefined);
  const highs = (f: (r: Reading) => number | null | undefined) => (pick(f).length ? Math.max(...pick(f)) : undefined);
  return {
    latest: newest(readings),
    tempMin: lows((r) => r.tempMin),
    tempMax: highs((r) => r.tempMax),
    rhMin: lows((r) => r.rhMin),
    rhMax: highs((r) => r.rhMax),
  };
}
