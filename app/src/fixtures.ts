/* A stored reading for tests: the protocol's worked example, decoded. */
import type { Reading } from './readings';

export const reading = (over: Partial<Reading>): Reading => ({
  stationId: 'station-001', sk: 'M#1792742400#0000000001', kind: 'minute', ts: 1792742400, tenths: 0,
  seq: 1, backfill: false, tempMean: 12.34, tempMin: 12.1, tempMax: 12.61, rhMean: 81.5, rhMin: 80.9, rhMax: 82.2,
  tempValidPct: 100, rhValidPct: 100, wetBulb: 10.33, flags: [], receivedAt: 1792742461123,
  ...over,
} as Reading);
