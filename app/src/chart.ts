/*
 * Chart geometry with D3, as plain data for React to draw. Every chart has one value axis: a
 * measure (temperature or humidity) per chart, one series per station. Minute summaries only.
 */
import { scaleLinear, scaleTime } from 'd3-scale';
import { area, line } from 'd3-shape';
import { takenAtMs, type Reading } from './readings';

export interface Size {
  width: number;
  height: number;
}

export interface Tick {
  at: number; // pixel position along the axis
  label: string;
}

export interface Series {
  id: string;
  readings: readonly Reading[]; // minute summaries, oldest first
}

export interface Measure {
  name: string;
  unit: string;
  mean: (r: Reading) => number | null | undefined;
  min: (r: Reading) => number | null | undefined;
  max: (r: Reading) => number | null | undefined;
  domain?: [number, number]; // fixed axis range, e.g. 0-100 %
}

export const TEMPERATURE: Measure = {
  name: 'Temperature', unit: '°C', mean: (r) => r.tempMean, min: (r) => r.tempMin, max: (r) => r.tempMax,
};
export const HUMIDITY: Measure = {
  name: 'Humidity', unit: '%', mean: (r) => r.rhMean, min: (r) => r.rhMin, max: (r) => r.rhMax, domain: [0, 100],
};

export const MARGIN = { top: 12, right: 16, bottom: 28, left: 44 };
/* Summaries come every minute; a longer gap means missing data, and the line breaks there. */
const GAP_MS = 2 * 60_000;

const hourFormat = new Intl.DateTimeFormat('en-GB', { hour: '2-digit', minute: '2-digit' });
const dayFormat = new Intl.DateTimeFormat('en-GB', { day: '2-digit', month: 'short' });

type Point = { t: number; mean: number; min: number; max: number } | null; // null breaks the line

function points(readings: readonly Reading[], m: Measure): Point[] {
  const out: Point[] = [];
  let lastT: number | undefined;
  for (const r of readings) {
    const t = takenAtMs(r);
    if (lastT !== undefined && t - lastT > GAP_MS) out.push(null);
    lastT = t;
    const [mean, min, max] = [m.mean(r), m.min(r), m.max(r)];
    out.push(mean != null && min != null && max != null ? { t, mean, min, max } : null);
  }
  return out;
}

export interface LineChart {
  lines: { id: string; line: string; band: string }[];
  xTicks: Tick[];
  yTicks: Tick[];
  plot: { left: number; right: number; top: number; bottom: number };
  xOf: (ms: number) => number;
  msAt: (px: number) => number;
}

export function buildLineChart(
  series: readonly Series[], m: Measure, fromMs: number, toMs: number, size: Size, withBand = false,
): LineChart {
  const plot = { left: MARGIN.left, right: size.width - MARGIN.right, top: MARGIN.top, bottom: size.height - MARGIN.bottom };
  const pts = series.map((s) => ({ id: s.id, points: points(s.readings, m) }));

  const values = pts.flatMap((s) => s.points.flatMap((p) => (p ? (withBand ? [p.min, p.max] : [p.mean]) : [])));
  const domain = m.domain ?? (values.length ? [Math.min(...values) - 1, Math.max(...values) + 1] : [0, 1]);

  const x = scaleTime().domain([fromMs, toMs]).range([plot.left, plot.right]);
  const y = scaleLinear().domain(domain).nice().range([plot.bottom, plot.top]);
  const defined = (p: Point) => p !== null;

  const lines = pts.map(({ id, points: ps }) => ({
    id,
    line: line<Point>().defined(defined).x((p) => x(p!.t)).y((p) => y(p!.mean))(ps) ?? '',
    band: withBand ? area<Point>().defined(defined).x((p) => x(p!.t)).y0((p) => y(p!.min)).y1((p) => y(p!.max))(ps) ?? '' : '',
  }));

  const multiDay = toMs - fromMs > 36 * 3600_000;
  return {
    lines,
    xTicks: x.ticks(6).map((d) => ({ at: x(d), label: (multiDay ? dayFormat : hourFormat).format(d) })),
    yTicks: y.ticks(5).map((v) => ({ at: y(v), label: `${v}${m.unit === '%' ? '%' : '°'}` })),
    plot,
    xOf: (ms) => x(ms),
    msAt: (px) => x.invert(px).getTime(),
  };
}

/** Each station's minute summary nearest to a time, if one lies within two minutes of it. */
export function valuesAt(series: readonly Series[], m: Measure, ms: number): { id: string; value: number; at: number }[] {
  return series.flatMap((s) => {
    let best: Reading | undefined;
    for (const r of s.readings) {
      if (m.mean(r) == null) continue;
      if (!best || Math.abs(takenAtMs(r) - ms) < Math.abs(takenAtMs(best) - ms)) best = r;
    }
    return best && Math.abs(takenAtMs(best) - ms) <= GAP_MS ? [{ id: s.id, value: m.mean(best)!, at: takenAtMs(best) }] : [];
  });
}
