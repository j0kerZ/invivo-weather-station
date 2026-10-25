/* The one filter row above a page's charts: it scopes every chart and table below it. */
import { RANGES, type RangeName } from '../range';

export function RangePicker({ value, onChange }: { value: RangeName; onChange: (name: RangeName) => void }) {
  return (
    <div className="filters" role="group" aria-label="Time range">
      {(Object.keys(RANGES) as RangeName[]).map((name) => (
        <button key={name} type="button" aria-pressed={name === value} onClick={() => onChange(name)}>Last {name}</button>
      ))}
    </div>
  );
}
