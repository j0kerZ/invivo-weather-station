/* Every station in one table: status, latest values and the range's extremes. A row opens it. */
import { ageText, formatRh, formatTemp, isOnline, rangeStats, type Reading } from '../readings';
import { colorOf } from '../stations';

export function StationsTable({ stations, now, onOpen }: {
  stations: Map<string, Reading[]>;
  now: number;
  onOpen: (stationId: string) => void;
}) {
  return (
    <div className="table-scroll">
      <table className="stations">
        <thead>
          <tr>
            <th>Station</th><th>Status</th><th>Temperature</th><th>Humidity</th><th>Wet-bulb</th>
            <th>Temperature min / max</th><th>Humidity min / max</th><th>Updated</th>
          </tr>
        </thead>
        <tbody>
          {[...stations].map(([id, readings]) => {
            const s = rangeStats(readings);
            const online = isOnline(s.lastReceivedAt, now);
            return (
              <tr key={id} tabIndex={0} onClick={() => onOpen(id)} onKeyDown={(e) => e.key === 'Enter' && onOpen(id)}>
                <td><span className="swatch" style={{ background: colorOf(id) }} />{id}</td>
                <td><span className={online ? 'badge online' : 'badge offline'}>{online ? '● Online' : '○ Offline'}</span></td>
                <td className="num">{formatTemp(s.latest?.tempMean)}</td>
                <td className="num">{formatRh(s.latest?.rhMean)}</td>
                <td className="num">{formatTemp(s.latest?.wetBulb)}</td>
                <td className="num">{formatTemp(s.tempMin)} / {formatTemp(s.tempMax)}</td>
                <td className="num">{formatRh(s.rhMin)} / {formatRh(s.rhMax)}</td>
                <td>{s.lastReceivedAt !== undefined ? ageText(now - s.lastReceivedAt) : 'no data'}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
