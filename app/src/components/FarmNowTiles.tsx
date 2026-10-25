/* The farm right now, from its online stations: four numbers above the charts. */
import { farmNow } from '../farm';
import { formatRh, formatTemp, type Reading } from '../readings';

export function FarmNowTiles({ stations, now }: { stations: Map<string, Reading[]>; now: number }) {
  const f = farmNow(stations, now);
  return (
    <section className="tiles" aria-label="Farm now">
      <div className="tile">
        <div className="label">Farm temperature now</div>
        <div className="value">{formatTemp(f.tempAvg)}</div>
        <div className="sub">average of online stations</div>
      </div>
      <div className="tile">
        <div className="label">Farm humidity now</div>
        <div className="value">{formatRh(f.rhAvg)}</div>
        <div className="sub">average of online stations</div>
      </div>
      <div className="tile">
        <div className="label">Coldest station now</div>
        <div className="value">{formatTemp(f.coldest?.temp)}</div>
        <div className="sub">{f.coldest?.stationId ?? 'no station online'}</div>
      </div>
      <div className="tile">
        <div className="label">Stations online</div>
        <div className="value">{f.online} of {f.total}</div>
        <div className="sub">offline after 2 min without data</div>
      </div>
    </section>
  );
}
