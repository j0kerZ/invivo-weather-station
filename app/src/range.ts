/* The time range every chart and table on a page uses, and loading readings for it. */
import { fetchReadings } from './api';
import type { Reading } from './readings';

export const RANGES = { '24 h': 24, '7 days': 24 * 7 } as const;
export type RangeName = keyof typeof RANGES;

/** A range as chosen: its name and the moment it was chosen, which fixes its start and end. */
export interface Range {
  name: RangeName;
  chosenAtMs: number;
}

export const chooseRange = (name: RangeName): Range => ({ name, chosenAtMs: Date.now() });
export const rangeStart = (r: Range) => Math.floor(r.chosenAtMs / 1000) - RANGES[r.name] * 3600;

/* Changes are shown in the records table only; the most recent ones are enough. */
const RECENT_CHANGES = 50;

/** Each station's minute summaries since fromTs, and its most recent changes. */
export async function loadRange(stationIds: readonly string[], fromTs: number): Promise<Reading[]> {
  const window = { fromTs, toTs: 4294967295 };
  const perStation = await Promise.all(
    stationIds.flatMap((stationId) => [
      fetchReadings({ ...window, stationId, kind: 'minute' }),
      fetchReadings({ ...window, stationId, kind: 'change', newestFirst: true, max: RECENT_CHANGES }),
    ]),
  );
  return perStation.flat();
}
