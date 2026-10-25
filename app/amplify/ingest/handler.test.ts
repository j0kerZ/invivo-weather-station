import { readFileSync } from 'node:fs';
import type { BatchWriteCommand } from '@aws-sdk/lib-dynamodb';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { FrameRejected, makeHandler, toItems, type IngestEvent, type ReadingItem } from './handler';
import { crc16 } from './protocol';

const vectors: { name: string; hex: string }[] = JSON.parse(
  readFileSync(new URL('../../../protocol/vectors-v1.json', import.meta.url), 'utf8'),
).vectors;
const vectorHex = (name: string) => vectors.find((v) => v.name === name)!.hex;

const RECEIVED_AT = 1792742461123;
const eventFor = (hex: string, stationId = 'station-001'): IngestEvent => ({
  payload: Buffer.from(hex, 'hex').toString('base64'),
  stationId,
  receivedAt: RECEIVED_AT,
});

/** A hand-written DynamoDB stand-in: records every batch, and can leave items unprocessed. */
class FakeDb {
  batches: ReadingItem[][] = [];
  /** How many items to leave unprocessed on each successive call (missing = none). */
  unprocessed: number[] = [];

  async send(command: BatchWriteCommand) {
    const requests = command.input.RequestItems!.Readings as { PutRequest: { Item: ReadingItem } }[];
    this.batches.push(requests.map((r) => r.PutRequest.Item));
    const keep = this.unprocessed.shift() ?? 0;
    return { UnprocessedItems: keep > 0 ? { Readings: requests.slice(-keep) } : {} };
  }
}

const noSleep = async () => {};

describe('toItems: frame to table rows', () => {
  it('stores a minute summary under its sequence number, in °C and %RH', () => {
    expect(toItems(eventFor(vectorHex('example_live')))).toEqual([{
      stationId: 'station-001',
      sk: 'M#1792742400#0000001042',
      kind: 'minute',
      ts: 1792742400,
      tenths: 0,
      seq: 1042,
      backfill: false,
      tempMean: 12.34,
      tempMin: 12.1,
      tempMax: 12.61,
      rhMean: 81.5,
      rhMin: 80.9,
      rhMax: 82.2,
      tempValidPct: 100,
      rhValidPct: 100,
      wetBulb: 10.33,
      flags: [],
      receivedAt: RECEIVED_AT,
    } satisfies ReadingItem]);
  });

  it('stores a change under its time to the tenth of a second, without a sequence number', () => {
    const [item] = toItems(eventFor(vectorHex('change_live_only')));
    expect(item.sk).toBe('C#1792742417.3');
    expect(item.kind).toBe('change');
    expect(item.tempMean).toBe(0.95);
    expect(item.rhMean).toBe(93.1);
    expect(item).not.toHaveProperty('seq');
    expect(item).not.toHaveProperty('backfill');
  });

  it('leaves out the values, and the wet-bulb, when a sensor had no valid sample', () => {
    const [item] = toItems(eventFor(vectorHex('temp_sensor_failed')));
    expect(item).not.toHaveProperty('tempMean');
    expect(item).not.toHaveProperty('wetBulb');
    expect(item.rhMean).toBe(81.5);
    expect(item.tempValidPct).toBe(0);
    expect(item.flags).toEqual(['TEMP_ERROR']);
  });

  it('marks backfilled summaries and numbers them from the frame', () => {
    const items = toItems(eventFor(vectorHex('backfill_high_seq')));
    expect(items.map((i) => [i.seq, i.backfill])).toEqual([
      [4294967280, true],
      [4294967281, true],
      [4294967282, true],
    ]);
    expect(items[0].flags).toEqual(['TIME_UNSYNCED']);
  });

  it('keeps one row when a frame names the same change twice', () => {
    const frame = Buffer.from(vectorHex('change_live_only'), 'hex');
    const record = frame.subarray(8, 29);
    const doubled = Buffer.concat([frame.subarray(0, 8), record, record, Buffer.alloc(2)]);
    doubled[2] = 2;
    doubled.writeUInt16LE(crc16(doubled.subarray(0, doubled.length - 2)), doubled.length - 2);
    expect(toItems(eventFor(doubled.toString('hex')))).toHaveLength(1);
  });

  it.each([
    ['third_record_invalid', 'RECORD_INVALID@2'],
    ['bad_frame_crc', 'FRAME_CRC'],
    ['unknown_version', 'VERSION'],
  ])('rejects %s with reason %s', (name, reason) => {
    expect(() => toItems(eventFor(vectorHex(name)))).toThrow(new FrameRejected(reason));
  });

  it('rejects a station ID that is not a valid thing name', () => {
    expect(() => toItems(eventFor(vectorHex('example_live'), '../station'))).toThrow(
      new FrameRejected('BAD_STATION_ID'),
    );
  });

  it('rejects an event without a payload', () => {
    expect(() => toItems({ stationId: 'station-001', receivedAt: RECEIVED_AT } as IngestEvent)).toThrow(
      new FrameRejected('BAD_EVENT'),
    );
  });
});

describe('handler: writing to DynamoDB', () => {
  beforeEach(() => {
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
  });
  afterEach(() => vi.restoreAllMocks());

  it('writes a 200-record frame in 8 batches of at most 25', async () => {
    const db = new FakeDb();
    await makeHandler(db, 'Readings', { sleep: noSleep })(eventFor(vectorHex('max_records')));
    expect(db.batches.map((b) => b.length)).toEqual([25, 25, 25, 25, 25, 25, 25, 25]);
    expect(new Set(db.batches.flat().map((i) => i.sk)).size).toBe(200);
  });

  it('retries the items DynamoDB did not take', async () => {
    const db = new FakeDb();
    db.unprocessed = [3, 1];
    await makeHandler(db, 'Readings', { sleep: noSleep })(eventFor(vectorHex('backfill_high_seq')));
    expect(db.batches.map((b) => b.length)).toEqual([3, 3, 1]);
  });

  it('waits a random time, at most 100, 200, 400 ms … capped at 2 s, before each retry', async () => {
    const waits: number[] = [];
    const sleep = async (ms: number) => { waits.push(ms); };
    for (const random of [() => 0.999999, () => 0.5, () => 0]) {
      const db = new FakeDb();
      db.unprocessed = [1, 1, 1, 1, 1, 1];
      await makeHandler(db, 'Readings', { sleep, random })(eventFor(vectorHex('example_live')));
    }
    expect(waits.map(Math.round)).toEqual([
      100, 200, 400, 800, 1600, 2000, // capped
      50, 100, 200, 400, 800, 1000,
      0, 0, 0, 0, 0, 0,
    ]);
  });

  it('fails after 7 attempts, so Lambda retries and then uses the failure queue', async () => {
    const db = new FakeDb();
    db.unprocessed = [1, 1, 1, 1, 1, 1, 1];
    await expect(makeHandler(db, 'Readings', { sleep: noSleep })(eventFor(vectorHex('example_live')))).rejects.toThrow(
      '1 items not written after 7 attempts',
    );
    expect(db.batches).toHaveLength(7);
  });

  it('fails on a malformed frame without touching the table, and logs the reason', async () => {
    const db = new FakeDb();
    await expect(makeHandler(db, 'Readings', { sleep: noSleep })(eventFor(vectorHex('bad_record_crc')))).rejects.toBeInstanceOf(
      FrameRejected,
    );
    expect(db.batches).toHaveLength(0);
    expect(console.warn).toHaveBeenCalledWith(
      JSON.stringify({ msg: 'frame rejected', stationId: 'station-001', reason: 'RECORD_CRC@0' }),
    );
  });
});
