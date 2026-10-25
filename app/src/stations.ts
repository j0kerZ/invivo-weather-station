/* The stations of this deployment: one per IoT thing, as listed in the backend outputs. */
import outputs from '../amplify_outputs.json';

export const STATIONS: readonly string[] = outputs.custom.iot.stations.map((s) => s.stationId);

/*
 * A station keeps its colour everywhere: categorical slot by its position in the station list.
 * Four validated slots; a farm with more stations needs small multiples instead of more colours.
 */
const SLOTS = 4;
export const colorOf = (stationId: string): string =>
  STATIONS.includes(stationId) ? `var(--series-${(STATIONS.indexOf(stationId) % SLOTS) + 1})` : 'var(--ink)'; // the farm average
