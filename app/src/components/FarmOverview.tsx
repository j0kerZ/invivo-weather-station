/*
 * The farm at a glance: the farm now, the farm average over the chosen range (band: coldest to
 * warmest station), every station's temperature and humidity, then the stations table. A table
 * row opens the station.
 */
import { HUMIDITY, TEMPERATURE } from '../chart';
import { farmMinutes } from '../farm';
import { byStation, type Reading } from '../readings';
import { loadRange, rangeStart, type Range, type RangeName } from '../range';
import { STATIONS } from '../stations';
import { useLiveReadings, useNow } from '../useLiveReadings';
import { FarmNowTiles } from './FarmNowTiles';
import { LineChart } from './LineChart';
import { RangePicker } from './RangePicker';
import { StationsTable } from './StationsTable';
import { Empty, ErrorBox, Loading } from './Status';

export function FarmOverview({ range, onRange, onOpen }: {
  range: Range;
  onRange: (name: RangeName) => void;
  onOpen: (stationId: string) => void;
}) {
  const fromTs = rangeStart(range);
  const loadedAtMs = range.chosenAtMs;
  const queryKey = `${range.name}@${range.chosenAtMs}`;
  const { state, reload } = useLiveReadings(STATIONS, queryKey, () => loadRange(STATIONS, fromTs), (r) => r.ts >= fromTs);

  if (STATIONS.length === 0) return <Empty message="No stations are registered in this deployment." />;

  return (
    <section className="farm">
      <RangePicker value={range.name} onChange={onRange} />
      {state.status === 'loading' && <Loading what="station data" />}
      {state.status === 'error' && <ErrorBox message={state.message} onRetry={reload} />}
      {state.status === 'ready' && (
        <FarmData readings={state.readings} fromMs={fromTs * 1000} toMs={loadedAtMs} dim={state.refreshing} onOpen={onOpen} />
      )}
      {state.status === 'ready' && state.liveError && (
        <p className="status error" role="alert">
          Live updates stopped ({state.liveError}). <button type="button" onClick={reload}>Reconnect</button>
        </p>
      )}
    </section>
  );
}

function FarmData({ readings, fromMs, toMs, dim, onOpen }: {
  readings: Reading[];
  fromMs: number;
  toMs: number;
  dim?: boolean;
  onOpen: (stationId: string) => void;
}) {
  const groups = byStation(STATIONS, readings);
  const series = STATIONS.map((id) => ({ id, readings: groups.get(id)!.filter((r) => r.kind === 'minute') }));
  const end = Math.max(toMs, ...readings.map((r) => r.ts * 1000));
  const hasMinutes = series.some((s) => s.readings.length > 0);
  const farm = [{ id: 'farm', readings: farmMinutes(groups) }];

  return (
    <div className={dim ? 'dim' : undefined}>
      <LiveFarmNow stations={groups} />
      {hasMinutes ? (
        <>
          <div className="charts">
            <LineChart title="Farm temperature (°C): average, band = coldest to warmest station" series={farm}
              measure={TEMPERATURE} fromMs={fromMs} toMs={end} band />
            <LineChart title="Farm humidity (%): average, band = driest to most humid station" series={farm}
              measure={HUMIDITY} fromMs={fromMs} toMs={end} band />
          </div>
          <div className="charts">
            <LineChart title="Temperature by station (°C)" series={series} measure={TEMPERATURE} fromMs={fromMs} toMs={end} />
            <LineChart title="Humidity by station (%)" series={series} measure={HUMIDITY} fromMs={fromMs} toMs={end} />
          </div>
        </>
      ) : (
        <Empty message="No readings from any station in this range." />
      )}
      <h2>Stations</h2>
      <LiveStationsTable stations={groups} onOpen={onOpen} />
    </div>
  );
}

/* Only the tiles and the table tick every second ("updated 12 s ago"), not the charts. */
function LiveFarmNow({ stations }: { stations: Map<string, Reading[]> }) {
  return <FarmNowTiles stations={stations} now={useNow()} />;
}

function LiveStationsTable(props: Omit<Parameters<typeof StationsTable>[0], 'now'>) {
  return <StationsTable {...props} now={useNow()} />;
}
