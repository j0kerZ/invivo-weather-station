import { describe, expect, it } from 'vitest';
import { buildLineChart, HUMIDITY, TEMPERATURE, valuesAt } from './chart';
import { reading } from './fixtures';

const SIZE = { width: 560, height: 240 };
const T0 = 1792742400;
const minute = (i: number, over = {}) => reading({ sk: `M#${T0 + i * 60}`, ts: T0 + i * 60, ...over });
const day = (from = T0) => [from * 1000, (from + 86400) * 1000] as const;

describe('buildLineChart', () => {
  it('draws one continuous line per station through consecutive minutes', () => {
    const g = buildLineChart([{ id: 'a', readings: [minute(0), minute(1), minute(2)] }], TEMPERATURE, ...day(), SIZE);
    expect(g.lines).toHaveLength(1);
    expect(g.lines[0].line.match(/M/g)).toHaveLength(1);
    expect(g.lines[0].band).toBe(''); // no band unless asked
  });

  it('breaks the line where minutes are missing, so a gap is not drawn over', () => {
    const g = buildLineChart([{ id: 'a', readings: [minute(0), minute(1), minute(10), minute(11)] }], TEMPERATURE, ...day(), SIZE);
    expect(g.lines[0].line.match(/M/g)).toHaveLength(2);
  });

  it('draws the min-max band when asked, and nothing for a sensor without data', () => {
    const banded = buildLineChart([{ id: 'a', readings: [minute(0), minute(1)] }], TEMPERATURE, ...day(), SIZE, true);
    expect(banded.lines[0].band).not.toBe('');
    const noTemp = { tempMean: null, tempMin: null, tempMax: null };
    const empty = buildLineChart([{ id: 'a', readings: [minute(0, noTemp), minute(1, noTemp)] }], TEMPERATURE, ...day(), SIZE);
    expect(empty.lines[0].line).toBe('');
  });

  it('keeps humidity on a fixed 0-100 % axis', () => {
    const g = buildLineChart([{ id: 'a', readings: [minute(0)] }], HUMIDITY, ...day(), SIZE);
    expect(g.yTicks[0].label).toBe('0%');
    expect(g.yTicks.at(-1)!.label).toBe('100%');
  });

  it('labels the time axis in hours for a day and in dates for a week, inside the plot', () => {
    const d = buildLineChart([], TEMPERATURE, ...day(), SIZE);
    expect(d.xTicks.every((t) => t.at >= d.plot.left && t.at <= d.plot.right)).toBe(true);
    expect(d.xTicks[0].label).toMatch(/^\d\d:\d\d$/);
    const w = buildLineChart([], TEMPERATURE, T0 * 1000, (T0 + 7 * 86400) * 1000, SIZE);
    expect(w.xTicks[0].label).toMatch(/^\d\d [A-Z][a-z]{2}$/);
  });
});

describe('valuesAt (the hover readout)', () => {
  const series = [
    { id: 'a', readings: [minute(0, { tempMean: 1 }), minute(1, { tempMean: 2 })] },
    { id: 'b', readings: [minute(1, { stationId: 'b', tempMean: 5 })] },
    { id: 'c', readings: [minute(30, { stationId: 'c', tempMean: 9 })] },
  ];

  it('gives every station its nearest value within two minutes, and skips stations without one', () => {
    expect(valuesAt(series, TEMPERATURE, (T0 + 70) * 1000)).toEqual([
      { id: 'a', value: 2, at: (T0 + 60) * 1000 },
      { id: 'b', value: 5, at: (T0 + 60) * 1000 },
    ]);
  });
});
