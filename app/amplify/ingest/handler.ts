/*
 * Ingest Lambda: invoked by the IoT rule for every MQTT message on invivo-ws/+/telemetry.
 * Decodes the binary frame and stores one DynamoDB item per record.
 *
 * Idempotent: a minute summary's key is its sequence number, a change's key is its time to the
 * tenth of a second (docs/PROTOCOL.md §6), so a frame delivered twice overwrites itself.
 */
import { DynamoDBClient } from '@aws-sdk/client-dynamodb';
import { BatchWriteCommand, DynamoDBDocumentClient } from '@aws-sdk/lib-dynamodb';
import {
  decodeFrame,
  FLAG_EVENT,
  FLAG_HEATER_ON,
  FLAG_RH_ERROR,
  FLAG_TEMP_ERROR,
  FLAG_TIME_UNSYNCED,
  FRAME_FLAG_BACKFILL,
  type RawRecord,
} from './protocol';
import { wetBulb } from './wet-bulb';

/** What the IoT rule sends (see the rule's SQL in stack.ts). */
export interface IngestEvent {
  payload: string;    // the binary frame, base64
  stationId: string;  // from the topic, i.e. the certificate's thing name: never from the payload
  receivedAt: number; // IoT Core receive time, ms
}

export interface ReadingItem {
  stationId: string;
  /* Minute summary: "M#<ts, 10 digits>#<seq, 10 digits>". Change: "C#<ts, 10 digits>.<tenths>". */
  sk: string;
  kind: 'minute' | 'change';
  ts: number;
  tenths: number;
  seq?: number;       // minute summaries only
  backfill?: boolean; // minute summaries only: resent after an outage
  tempMean?: number;  // °C; absent when no temperature sample was valid
  tempMin?: number;
  tempMax?: number;
  rhMean?: number;    // %RH; absent when no humidity sample was valid
  rhMin?: number;
  rhMax?: number;
  tempValidPct: number;
  rhValidPct: number;
  wetBulb?: number;   // °C, from the mean temperature and humidity; absent outside the formula's range
  flags: string[];
  receivedAt: number;
}

/** A frame that can never be stored. Thrown so the event ends up in the failure queue. */
export class FrameRejected extends Error {
  constructor(readonly reason: string) {
    super(`frame rejected: ${reason}`);
    this.name = 'FrameRejected';
  }
}

const STATION_ID = /^[a-z0-9-]{3,64}$/;
const BATCH_SIZE = 25; // DynamoDB BatchWriteItem limit
const MAX_ATTEMPTS = 4;
const BACKOFF_BASE_MS = 50;
const FLAG_NAMES: [number, string][] = [
  [FLAG_TIME_UNSYNCED, 'TIME_UNSYNCED'],
  [FLAG_TEMP_ERROR, 'TEMP_ERROR'],
  [FLAG_RH_ERROR, 'RH_ERROR'],
  [FLAG_HEATER_ON, 'HEATER_ON'],
];

const pad10 = (n: number) => String(n).padStart(10, '0');
const round2 = (n: number) => Math.round(n * 100) / 100;

function toItem(stationId: string, seq: number, r: RawRecord, backfill: boolean, receivedAt: number): ReadingItem {
  const isChange = (r.flags & FLAG_EVENT) !== 0;
  const item: ReadingItem = {
    stationId,
    sk: isChange ? `C#${pad10(r.ts)}.${r.tenths}` : `M#${pad10(r.ts)}#${pad10(seq)}`,
    kind: isChange ? 'change' : 'minute',
    ts: r.ts,
    tenths: r.tenths,
    tempValidPct: r.tempValidPct,
    rhValidPct: r.rhValidPct,
    flags: FLAG_NAMES.filter(([bit]) => r.flags & bit).map(([, name]) => name),
    receivedAt,
  };
  if (!isChange) {
    item.seq = seq;
    item.backfill = backfill;
  }
  if (r.tempValidPct > 0) {
    item.tempMean = r.tMean / 100;
    item.tempMin = r.tMin / 100;
    item.tempMax = r.tMax / 100;
  }
  if (r.rhValidPct > 0) {
    item.rhMean = r.rhMean / 100;
    item.rhMin = r.rhMin / 100;
    item.rhMax = r.rhMax / 100;
  }
  if (item.tempMean !== undefined && item.rhMean !== undefined) {
    const tw = wetBulb(item.tempMean, item.rhMean);
    if (tw !== undefined) item.wetBulb = round2(tw);
  }
  return item;
}

/** Validates the event and decodes its frame. Pure: no AWS calls. Throws FrameRejected. */
export function toItems(event: IngestEvent): ReadingItem[] {
  if (typeof event?.stationId !== 'string' || !STATION_ID.test(event.stationId)) {
    throw new FrameRejected('BAD_STATION_ID');
  }
  if (typeof event.payload !== 'string' || !Number.isFinite(event.receivedAt)) {
    throw new FrameRejected('BAD_EVENT');
  }
  const decoded = decodeFrame(new Uint8Array(Buffer.from(event.payload, 'base64')));
  if (!decoded.ok) {
    throw new FrameRejected(
      decoded.badRecord === undefined ? decoded.reason : `${decoded.reason}@${decoded.badRecord}`,
    );
  }
  const { frameFlags, seqFirst, records } = decoded.frame;
  const backfill = (frameFlags & FRAME_FLAG_BACKFILL) !== 0;
  const items = records.map((r, i) => toItem(event.stationId, seqFirst + i, r, backfill, event.receivedAt));

  /* DynamoDB refuses a batch that names the same key twice; the last record wins, as a resend would. */
  return [...new Map(items.map((item) => [item.sk, item])).values()];
}

type Sender = Pick<DynamoDBDocumentClient, 'send'>;

/** Injected so tests run without real waiting or randomness. */
export interface Timing {
  sleep?: (ms: number) => Promise<unknown>;
  random?: () => number; // in [0, 1)
}

export function makeHandler(
  db: Sender,
  tableName: string,
  { sleep = (ms) => new Promise((r) => setTimeout(r, ms)), random = Math.random }: Timing = {},
) {
  /*
   * Writes up to 25 items. DynamoDB may accept part of a batch under load; the rest is retried
   * with exponential backoff and full jitter (a random wait up to 100, 200, 400 ms), so stations
   * that reconnect together after an outage do not retry in lockstep.
   */
  async function writeBatch(items: ReadingItem[]): Promise<void> {
    let requests = items.map((Item) => ({ PutRequest: { Item } }));
    for (let attempt = 0; requests.length > 0; attempt++) {
      if (attempt === MAX_ATTEMPTS) {
        /* Throwing makes Lambda retry the whole (idempotent) event, then use the failure queue. */
        throw new Error(`${requests.length} items not written after ${attempt} attempts`);
      }
      if (attempt > 0) await sleep(random() * BACKOFF_BASE_MS * 2 ** attempt);
      const out = await db.send(new BatchWriteCommand({ RequestItems: { [tableName]: requests } }));
      requests = (out.UnprocessedItems?.[tableName] ?? []) as typeof requests;
    }
  }

  return async (event: IngestEvent): Promise<void> => {
    let items: ReadingItem[];
    try {
      items = toItems(event);
    } catch (err) {
      if (err instanceof FrameRejected) {
        console.warn(JSON.stringify({ msg: 'frame rejected', stationId: event?.stationId, reason: err.reason }));
      }
      /* Lambda retries, then hands the event to the failure queue, where it is kept as evidence. */
      throw err;
    }

    for (let i = 0; i < items.length; i += BATCH_SIZE) {
      await writeBatch(items.slice(i, i + BATCH_SIZE));
    }

    console.log(JSON.stringify({
      msg: 'stored',
      stationId: event.stationId,
      minutes: items.filter((item) => item.kind === 'minute').length,
      changes: items.filter((item) => item.kind === 'change').length,
      firstSeq: items.find((item) => item.kind === 'minute')?.seq,
    }));
  };
}

export const handler = makeHandler(
  DynamoDBDocumentClient.from(new DynamoDBClient({})),
  process.env.TABLE_NAME ?? '',
);
