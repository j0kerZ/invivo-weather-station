/* A station's latest values, how fresh they are, and whether it is online. */
import { ageText, formatRh, formatTemp, isOnline, lastReceivedAt, latest, type Reading } from '../readings';
import type { LiveState } from '../useLiveReadings';
import { Empty, ErrorBox, Loading } from './Status';

export function StationCardView({ stationId, state, now, onOpen, onRetry }: {
  stationId: string;
  state: LiveState;
  now: number;
  onOpen?: () => void; // absent on the station's own page
  onRetry: () => void;
}) {
  const readings = state.status === 'ready' ? state.readings : [];
  const current: Reading | undefined = latest(readings);
  const received = lastReceivedAt(readings);
  const online = isOnline(received, now);
  return (
    <article className="card">
      <header>
        <h2>{stationId}</h2>
        {state.status === 'ready' && (
          <span className={online ? 'badge online' : 'badge offline'}>{online ? 'Online' : 'Offline'}</span>
        )}
      </header>
      {state.status === 'loading' && <Loading what="latest reading" />}
      {state.status === 'error' && <ErrorBox message={state.message} onRetry={onRetry} />}
      {state.status === 'ready' && !current && <Empty message="No readings yet for this station." />}
      {current && received !== undefined && (
        <>
          <dl className="values">
            <div><dt>Temperature</dt><dd>{formatTemp(current.tempMean)}</dd></div>
            <div><dt>Humidity</dt><dd>{formatRh(current.rhMean)}</dd></div>
            <div><dt>Wet-bulb</dt><dd>{formatTemp(current.wetBulb)}</dd></div>
          </dl>
          <p className="muted">Updated {ageText(now - received)}</p>
        </>
      )}
      {onOpen && <button type="button" onClick={onOpen}>Open station</button>}
    </article>
  );
}
