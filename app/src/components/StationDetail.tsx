/* One station: its latest values, temperature and humidity over the range, and recent records. */
import { HUMIDITY, TEMPERATURE } from '../chart';
import { takenAtMs, type Reading } from '../readings';
import { loadRange, rangeStart, type Range, type RangeName } from '../range';
import { useLiveReadings, useNow } from '../useLiveReadings';
import { LineChart } from './LineChart';
import { RangePicker } from './RangePicker';
import { ReadingsTable } from './ReadingsTable';
import { StationCardView } from './StationCard';
import { Empty, ErrorBox, Loading } from './Status';

const TABLE_ROWS = 50;

export function StationDetail({ stationId, range, onRange, onBack }: {
  stationId: string;
  range: Range;
  onRange: (name: RangeName) => void;
  onBack: () => void;
}) {
  const fromTs = rangeStart(range);
  const loadedAtMs = range.chosenAtMs;
  const queryKey = `${range.name}@${range.chosenAtMs}`;
  const { state, reload } = useLiveReadings([stationId], queryKey, () => loadRange([stationId], fromTs), (r) => r.ts >= fromTs);

  return (
    <section className="detail">
      <p><button type="button" className="link" onClick={onBack}>← Farm overview</button></p>
      <RangePicker value={range.name} onChange={onRange} />
      <LiveStationCard stationId={stationId} state={state} onRetry={reload} />
      {state.status === 'loading' && <Loading what="history" />}
      {state.status === 'error' && <ErrorBox message={state.message} onRetry={reload} />}
      {state.status === 'ready' && (
        <div className={state.refreshing ? 'dim' : undefined}>
          <History stationId={stationId} readings={state.readings} fromMs={fromTs * 1000} toMs={loadedAtMs} />
        </div>
      )}
    </section>
  );
}

function History({ stationId, readings, fromMs, toMs }: { stationId: string; readings: Reading[]; fromMs: number; toMs: number }) {
  if (readings.length === 0) return <Empty message="No readings in this range." />;
  const series = [{ id: stationId, readings: readings.filter((r) => r.kind === 'minute') }];
  const end = Math.max(toMs, ...readings.map((r) => r.ts * 1000));
  const recent = [...readings].sort((a, b) => takenAtMs(b) - takenAtMs(a)).slice(0, TABLE_ROWS);
  return (
    <>
      <div className="charts">
        <LineChart title="Temperature (°C): mean, band = minute min to max" series={series} measure={TEMPERATURE} fromMs={fromMs} toMs={end} band />
        <LineChart title="Humidity (%): mean, band = minute min to max" series={series} measure={HUMIDITY} fromMs={fromMs} toMs={end} band />
      </div>
      <h2>Recent records</h2>
      <ReadingsTable readings={recent} />
    </>
  );
}

/* Only the latest-values card ticks every second ("updated 12 s ago"), not the charts. */
function LiveStationCard(props: Omit<Parameters<typeof StationCardView>[0], 'now'>) {
  return <StationCardView {...props} now={useNow()} />;
}
