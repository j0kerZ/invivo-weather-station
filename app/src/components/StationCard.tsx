/* A station's latest values, how fresh they are, and whether it is online. */
import { ageText, formatRh, formatTemp, isOnline, newest, type Reading } from '../readings';
import type { LiveState } from '../useLiveReadings';
import { Empty, ErrorBox, Loading } from './Status';

export function StationCardView({ stationId, state, now, onOpen, onRetry }: {
  stationId: string;
  state: LiveState;
  now: number;
  onOpen?: () => void; // absent on the station's own page
  onRetry: () => void;
}) {
  const latest: Reading | undefined = state.status === 'ready' ? newest(state.readings) : undefined;
  const online = isOnline(latest, now);
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
      {state.status === 'ready' && !latest && <Empty message="No readings yet for this station." />}
      {latest && (
        <>
          <dl className="values">
            <div><dt>Temperature</dt><dd>{formatTemp(latest.tempMean)}</dd></div>
            <div><dt>Humidity</dt><dd>{formatRh(latest.rhMean)}</dd></div>
            <div><dt>Wet-bulb</dt><dd>{formatTemp(latest.wetBulb)}</dd></div>
          </dl>
          <p className="muted">Updated {ageText(now - latest.receivedAt)}</p>
        </>
      )}
      {onOpen && <button type="button" onClick={onOpen}>Open station</button>}
    </article>
  );
}
