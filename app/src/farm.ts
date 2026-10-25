/*
 * The farm as a whole, from its stations. Minute summaries start on whole minutes at every
 * station (PROTOCOL.md §2), so each minute has at most one value per station and averaging is
 * exact, with no resampling.
 */
import { isOnline, newest, type Reading } from './readings';

const avg = (vs: number[]) => vs.reduce((s, v) => s + v, 0) / vs.length;
const present = (vs: (number | null | undefined)[]) => vs.filter((v): v is number => v !== null && v !== undefined);

/*
 * One reading per minute for the whole farm: the mean of the stations that reported that minute,
 * and as min/max the coldest and warmest station (driest and most humid for humidity). Shaped as
 * a Reading so the same charts draw it.
 */
export function farmMinutes(stations: Map<string, Reading[]>): Reading[] {
  const byTs = new Map<number, Reading[]>();
  for (const readings of stations.values()) {
    for (const r of readings) {
      if (r.kind !== 'minute') continue;
      const list = byTs.get(r.ts) ?? [];
      list.push(r);
      byTs.set(r.ts, list);
    }
  }
  return [...byTs.keys()].sort((a, b) => a - b).map((ts) => {
    const rs = byTs.get(ts)!;
    const temps = present(rs.map((r) => r.tempMean));
    const rhs = present(rs.map((r) => r.rhMean));
    return {
      stationId: 'farm', sk: `M#${ts}`, kind: 'minute', ts, tenths: 0, flags: [], tempValidPct: 100, rhValidPct: 100,
      receivedAt: Math.max(...rs.map((r) => r.receivedAt)),
      tempMean: temps.length ? avg(temps) : null, tempMin: temps.length ? Math.min(...temps) : null, tempMax: temps.length ? Math.max(...temps) : null,
      rhMean: rhs.length ? avg(rhs) : null, rhMin: rhs.length ? Math.min(...rhs) : null, rhMax: rhs.length ? Math.max(...rhs) : null,
    } as Reading;
  });
}

export interface FarmNow {
  online: number;
  total: number;
  tempAvg?: number;
  rhAvg?: number;
  coldest?: { stationId: string; temp: number };
}

/** The farm right now, from the stations that are online. */
export function farmNow(stations: Map<string, Reading[]>, nowMs: number): FarmNow {
  const latest = [...stations].map(([stationId, rs]) => ({ stationId, r: newest(rs) }))
    .filter((s): s is { stationId: string; r: Reading } => s.r !== undefined && isOnline(s.r, nowMs));
  const temps = latest.filter((s) => s.r.tempMean != null).map((s) => ({ stationId: s.stationId, temp: s.r.tempMean! }));
  const rhs = present(latest.map((s) => s.r.rhMean));
  return {
    online: latest.length,
    total: stations.size,
    tempAvg: temps.length ? avg(temps.map((t) => t.temp)) : undefined,
    rhAvg: rhs.length ? avg(rhs) : undefined,
    coldest: temps.length ? temps.reduce((c, t) => (t.temp < c.temp ? t : c)) : undefined,
  };
}
