/* The card in each of its states, rendered to HTML. */
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { reading } from '../fixtures';
import { StationCardView } from './StationCard';

const NOW = 1792742461123 + 12_000;
const render = (state: Parameters<typeof StationCardView>[0]['state'], onOpen?: () => void) =>
  renderToStaticMarkup(<StationCardView stationId="station-001" state={state} now={NOW} onOpen={onOpen} onRetry={() => {}} />);

describe('StationCardView', () => {
  it('shows loading', () => expect(render({ status: 'loading' })).toContain('Loading latest reading'));

  it('shows the error with a way to retry', () => {
    const html = render({ status: 'error', message: 'Network error' });
    expect(html).toContain('Could not load the data: Network error');
    expect(html).toContain('Try again');
  });

  it('says so when a station has no readings, and marks it offline', () => {
    const html = render({ status: 'ready', readings: [] });
    expect(html).toContain('No readings yet for this station.');
    expect(html).toContain('Offline');
  });

  it('shows the latest values, their age and online status', () => {
    const html = render({ status: 'ready', readings: [reading({})] }, () => {});
    expect(html).toContain('12.3 °C');
    expect(html).toContain('81.5 %');
    expect(html).toContain('Updated 12 s ago');
    expect(html).toContain('Online');
    expect(html).toContain('Open station');
  });

  it('has no open button on the station page itself', () => {
    expect(render({ status: 'ready', readings: [reading({})] })).not.toContain('Open station');
  });
});
