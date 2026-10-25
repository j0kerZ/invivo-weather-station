import { describe, expect, it } from 'vitest';
import { reading } from './fixtures';
import { ageText, delayMs, formatTemp, isOnline, mergeReadings, newest, OFFLINE_AFTER_MS, takenAtMs } from './readings';

describe('time and delay', () => {
  it('takes a change at its tenth of a second, a summary at its minute start', () => {
    expect(takenAtMs(reading({ kind: 'change', ts: 1792742417, tenths: 3 }))).toBe(1792742417300);
    expect(takenAtMs(reading({}))).toBe(1792742400000);
  });

  it('measures a summary from the end of its minute, a change from its sample', () => {
    expect(delayMs(reading({}))).toBe(1123);
    expect(delayMs(reading({ kind: 'change', ts: 1792742417, tenths: 3, receivedAt: 1792742417340 }))).toBe(40);
  });
});

describe('isOnline', () => {
  const r = reading({ receivedAt: 1_000_000 });
  it('is online until two minutes without data', () => {
    expect(isOnline(r, 1_000_000 + OFFLINE_AFTER_MS)).toBe(true);
    expect(isOnline(r, 1_000_000 + OFFLINE_AFTER_MS + 1)).toBe(false);
  });
  it('is offline with no reading at all', () => expect(isOnline(undefined, 0)).toBe(false));
});

describe('mergeReadings', () => {
  it('keeps one row per key (a redelivery replaces), sorted by time', () => {
    const a = reading({ sk: 'M#2', ts: 1792742460, receivedAt: 1 });
    const b = reading({ sk: 'M#1', ts: 1792742400 });
    const aAgain = reading({ sk: 'M#2', ts: 1792742460, receivedAt: 2 });
    expect(mergeReadings([a, b], [aAgain]).map((r) => [r.sk, r.receivedAt])).toEqual([['M#1', 1792742461123], ['M#2', 2]]);
  });
  it('newest is the most recently received', () => {
    expect(newest([reading({ sk: 'x', receivedAt: 5 }), reading({ sk: 'y', receivedAt: 9 })])?.sk).toBe('y');
    expect(newest([])).toBeUndefined();
  });
});

describe('formatting', () => {
  it.each([[0, 'just now'], [12_000, '12 s ago'], [180_000, '3 min ago'], [7_200_000, '2 h ago'], [172_800_000, '2 d ago']])(
    'age %i ms → %s', (ms, text) => expect(ageText(ms)).toBe(text));
  it('shows n/a for a sensor without data', () => {
    expect(formatTemp(null)).toBe('n/a');
    expect(formatTemp(-1.25)).toBe('-1.3 °C');
  });
});
