import { describe, expect, it } from 'vitest';
import { farmMinutes, farmNow } from './farm';
import { reading } from './fixtures';

const T0 = 1792742400;
const at = (stationId: string, i: number, tempMean: number | null, rhMean: number, receivedAt = 0) =>
  reading({ stationId, sk: `M#${T0 + i * 60}`, ts: T0 + i * 60, tempMean, rhMean, receivedAt });

describe('farmMinutes', () => {
  const stations = new Map([
    ['a', [at('a', 0, 2, 80), at('a', 1, 4, 70)]],
    ['b', [at('b', 0, 6, 60)]],
  ]);

  it('averages the stations that reported each minute; min/max are the coldest and warmest station', () => {
    const [m0, m1] = farmMinutes(stations);
    expect([m0.ts, m0.tempMean, m0.tempMin, m0.tempMax]).toEqual([T0, 4, 2, 6]);
    expect([m0.rhMean, m0.rhMin, m0.rhMax]).toEqual([70, 60, 80]);
    expect([m1.ts, m1.tempMean, m1.tempMin, m1.tempMax]).toEqual([T0 + 60, 4, 4, 4]); // only a reported
  });

  it('leaves out a station whose temperature sensor had no data, but keeps its humidity', () => {
    const [m] = farmMinutes(new Map([['a', [at('a', 0, null, 80)]], ['b', [at('b', 0, 6, 60)]]]));
    expect([m.tempMean, m.rhMean]).toEqual([6, 70]);
  });

  it('ignores change records', () => {
    expect(farmMinutes(new Map([['a', [reading({ kind: 'change' })]]]))).toEqual([]);
  });
});

describe('farmNow', () => {
  const now = 1_000_000_000;
  it('counts online stations and summarises only them', () => {
    const stations = new Map([
      ['a', [at('a', 0, 2, 80, now - 10_000)]],
      ['b', [at('b', 0, 6, 60, now - 20_000)]],
      ['c', [at('c', 0, -9, 99, now - 600_000)]], // offline: not counted
      ['d', []],
    ]);
    expect(farmNow(stations, now)).toEqual({ online: 2, total: 4, tempAvg: 4, rhAvg: 70, coldest: { stationId: 'a', temp: 2 } });
  });

  it('uses the reading taken last while a backlog upload delivers older readings later', () => {
    const live = at('a', 10, 18, 55, now - 30_000);
    const backlog = at('a', 0, 6, 90, now - 1_000); // a night-time reading, arriving after the live one
    expect(farmNow(new Map([['a', [live, backlog]]]), now)).toMatchObject({ online: 1, tempAvg: 18, rhAvg: 55 });
  });

  it('has no values when every station is offline', () => {
    expect(farmNow(new Map([['a', []]]), now)).toEqual({ online: 0, total: 1, tempAvg: undefined, rhAvg: undefined, coldest: undefined });
  });
});
