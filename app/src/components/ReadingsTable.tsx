/*
 * Recent records with the fields an engineer needs to debug the data path: sequence numbers (gaps
 * mean missing summaries), sample validity, flags, backfill, and the station-to-cloud delay.
 */
import { delayMs, formatRh, formatTemp, formatTime, takenAtMs, type Reading } from '../readings';

const range = (min?: number | null, mean?: number | null, max?: number | null, f = formatTemp) =>
  mean === null || mean === undefined ? 'n/a' : min === max ? f(mean) : `${f(min)} / ${f(mean)} / ${f(max)}`;

export function ReadingsTable({ readings }: { readings: readonly Reading[] }) {
  return (
    <div className="table-scroll">
      <table>
        <thead>
          <tr>
            <th>Taken</th><th>Kind</th><th>Seq</th><th>Temperature min / mean / max</th>
            <th>Humidity min / mean / max</th><th>Wet-bulb</th><th>Valid T / RH</th><th>Flags</th><th>Delay</th>
          </tr>
        </thead>
        <tbody>
          {readings.map((r) => (
            <tr key={r.sk}>
              <td>{formatTime(takenAtMs(r))}</td>
              <td>{r.kind === 'minute' ? (r.backfill ? 'minute (backfill)' : 'minute') : 'change'}</td>
              <td className="num">{r.seq ?? ''}</td>
              <td>{range(r.tempMin, r.tempMean, r.tempMax)}</td>
              <td>{range(r.rhMin, r.rhMean, r.rhMax, formatRh)}</td>
              <td>{formatTemp(r.wetBulb)}</td>
              <td className="num">{r.tempValidPct}% / {r.rhValidPct}%</td>
              <td>{r.flags.filter(Boolean).join(', ')}</td>
              <td className="num">{(delayMs(r) / 1000).toFixed(1)} s</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
