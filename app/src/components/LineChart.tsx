/*
 * One measure over time, one line per station, one value axis. Hovering shows a crosshair and
 * every station's value at that time.
 */
import { useState, type PointerEvent } from 'react';
import { buildLineChart, valuesAt, type Measure, type Series } from '../chart';
import { formatTime } from '../readings';
import { colorOf } from '../stations';

const SIZE = { width: 560, height: 240 };

export function LineChart({ title, series, measure, fromMs, toMs, band = false }: {
  title: string;
  series: readonly Series[];
  measure: Measure;
  fromMs: number;
  toMs: number;
  band?: boolean;
}) {
  const g = buildLineChart(series, measure, fromMs, toMs, SIZE, band);
  const [hoverMs, setHoverMs] = useState<number>();
  const hovered = hoverMs === undefined ? [] : valuesAt(series, measure, hoverMs);

  const onMove = (e: PointerEvent<SVGRectElement>) => {
    const box = e.currentTarget.ownerSVGElement!.getBoundingClientRect();
    setHoverMs(g.msAt(((e.clientX - box.left) / box.width) * SIZE.width));
  };

  return (
    <figure className="chart">
      <figcaption>
        <h3>{title}</h3>
        {series.length > 1 && (
          <ul className="legend">
            {series.map((s) => <li key={s.id}><span className="line-key" style={{ background: colorOf(s.id) }} />{s.id}</li>)}
          </ul>
        )}
      </figcaption>
      <div className="plot">
        <svg viewBox={`0 0 ${SIZE.width} ${SIZE.height}`} role="img" aria-label={`${title}, ${series.map((s) => s.id).join(', ')}`}>
          {g.yTicks.map((t) => (
            <g key={`y${t.at}`}>
              <line className="grid" x1={g.plot.left} x2={g.plot.right} y1={t.at} y2={t.at} />
              <text className="tick" x={g.plot.left - 6} y={t.at + 4} textAnchor="end">{t.label}</text>
            </g>
          ))}
          {g.xTicks.map((t) => (
            <text key={`x${t.at}`} className="tick" x={t.at} y={g.plot.bottom + 18} textAnchor="middle">{t.label}</text>
          ))}
          {g.lines.map((l) => (
            <g key={l.id} style={{ color: colorOf(l.id) }}>
              {l.band && <path className="band" d={l.band} />}
              <path className="series" d={l.line} />
            </g>
          ))}
          {hoverMs !== undefined && hovered.length > 0 && (
            <line className="crosshair" x1={g.xOf(hovered[0].at)} x2={g.xOf(hovered[0].at)} y1={g.plot.top} y2={g.plot.bottom} />
          )}
          <rect className="hit" x={g.plot.left} y={g.plot.top} width={g.plot.right - g.plot.left}
            height={g.plot.bottom - g.plot.top} onPointerMove={onMove} onPointerLeave={() => setHoverMs(undefined)} />
        </svg>
        {hovered.length > 0 && (
          <div className={g.xOf(hovered[0].at) > SIZE.width * 0.6 ? 'tooltip flip' : 'tooltip'}
            style={{ left: `${(g.xOf(hovered[0].at) / SIZE.width) * 100}%` }}>
            <div className="muted">{formatTime(hovered[0].at)}</div>
            {hovered.map((h) => (
              <div key={h.id}>
                <span className="line-key" style={{ background: colorOf(h.id) }} />
                <strong>{h.value.toFixed(1)} {measure.unit}</strong> <span className="muted">{h.id}</span>
              </div>
            ))}
          </div>
        )}
      </div>
    </figure>
  );
}
