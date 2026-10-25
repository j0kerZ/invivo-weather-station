/*
 * Weather station binary protocol v1 decoder (docs/PROTOCOL.md). Mirrors firmware/src/ws_protocol.c:
 * same checks, same order, same reason codes. Both are tested against protocol/vectors-v1.json.
 */

export const PROTOCOL_VERSION = 1;
export const RECORD_SIZE = 21;
const RECORD_CRC_OFFSET = 20;
const HEADER_SIZE = 8;
const CRC_SIZE = 2;
export const MAX_RECORDS = 200;
const VALID_PCT_MAX = 100;

const TEMP_MIN = -5500;
const TEMP_MAX = 15000;
const TEMP_INVALID = -32768;
const RH_MAX = 10000;
const RH_INVALID = 0xffff;

export const FLAG_EVENT = 1 << 0;
export const FLAG_TIME_UNSYNCED = 1 << 1;
export const FLAG_TEMP_ERROR = 1 << 2;
export const FLAG_RH_ERROR = 1 << 3;
export const FLAG_HEATER_ON = 1 << 4;
export const FRAME_FLAG_BACKFILL = 1 << 0;
export const FRAME_FLAG_LIVE_ONLY = 1 << 1;

export type ReasonCode =
  | 'LENGTH'
  | 'VERSION'
  | 'FRAME_CRC'
  | 'COUNT'
  | 'RECORD_CRC'
  | 'RECORD_INVALID';

/** A record in protocol units: 0.01 °C, 0.01 %RH, Unix seconds plus tenths. */
export interface RawRecord {
  ts: number;
  tenths: number;
  tMean: number;
  tMin: number;
  tMax: number;
  rhMean: number;
  rhMin: number;
  rhMax: number;
  tempValidPct: number;
  rhValidPct: number;
  flags: number;
}

export interface Frame {
  version: number;
  frameFlags: number;
  seqFirst: number;
  records: RawRecord[];
}

export type DecodeResult =
  | { ok: true; frame: Frame }
  | { ok: false; reason: ReasonCode; badRecord?: number };

/** CRC-8/NRSC-5: poly 0x31, init 0xFF. */
export function crc8(data: Uint8Array): number {
  let crc = 0xff;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      crc = crc & 0x80 ? ((crc << 1) ^ 0x31) & 0xff : (crc << 1) & 0xff;
    }
  }
  return crc;
}

/** CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF. */
export function crc16(data: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

/* One quantity's statistics (temperature or humidity). */
function statsValid(
  pct: number, mean: number, min: number, max: number,
  invalid: number, lo: number, hi: number, isEvent: boolean,
): boolean {
  if (pct === 0) return mean === invalid && min === invalid && max === invalid;
  /* A summary may be partly valid; a change event is one sample: valid or not. */
  if (pct > VALID_PCT_MAX || (isEvent && pct !== VALID_PCT_MAX)) return false;
  if (min < lo || max > hi || min > mean || mean > max) return false;
  return !isEvent || (min === mean && mean === max);
}

/** The §3 rules: sentinels, ranges, min <= mean <= max, event shape. */
export function recordIsValid(r: RawRecord): boolean {
  const isEvent = (r.flags & FLAG_EVENT) !== 0;
  if (r.tenths > 9) return false;
  if (isEvent && r.tempValidPct === 0 && r.rhValidPct === 0) return false;
  return (
    statsValid(r.tempValidPct, r.tMean, r.tMin, r.tMax, TEMP_INVALID, TEMP_MIN, TEMP_MAX, isEvent) &&
    statsValid(r.rhValidPct, r.rhMean, r.rhMin, r.rhMax, RH_INVALID, 0, RH_MAX, isEvent)
  );
}

function decodeRecord(bytes: Uint8Array): RawRecord | 'RECORD_CRC' | 'RECORD_INVALID' {
  if (crc8(bytes.subarray(0, RECORD_CRC_OFFSET)) !== bytes[RECORD_CRC_OFFSET]) return 'RECORD_CRC';
  const v = new DataView(bytes.buffer, bytes.byteOffset, RECORD_SIZE);
  const r: RawRecord = {
    ts: v.getUint32(0, true),
    tenths: v.getUint8(4),
    tMean: v.getInt16(5, true),
    tMin: v.getInt16(7, true),
    tMax: v.getInt16(9, true),
    rhMean: v.getUint16(11, true),
    rhMin: v.getUint16(13, true),
    rhMax: v.getUint16(15, true),
    tempValidPct: v.getUint8(17),
    rhValidPct: v.getUint8(18),
    flags: v.getUint8(19),
  };
  return recordIsValid(r) ? r : 'RECORD_INVALID';
}

/** Validates a whole frame in the order of PROTOCOL.md §8. */
export function decodeFrame(buf: Uint8Array): DecodeResult {
  if (buf.length < HEADER_SIZE + CRC_SIZE) return { ok: false, reason: 'LENGTH' };
  if (buf[0] !== PROTOCOL_VERSION) return { ok: false, reason: 'VERSION' };
  const view = new DataView(buf.buffer, buf.byteOffset, buf.length);
  if (crc16(buf.subarray(0, buf.length - CRC_SIZE)) !== view.getUint16(buf.length - CRC_SIZE, true)) {
    return { ok: false, reason: 'FRAME_CRC' };
  }
  const count = buf[2];
  if (count === 0 || count > MAX_RECORDS) return { ok: false, reason: 'COUNT' };
  if (buf.length !== HEADER_SIZE + RECORD_SIZE * count + CRC_SIZE) return { ok: false, reason: 'LENGTH' };

  const records: RawRecord[] = [];
  for (let i = 0; i < count; i++) {
    const offset = HEADER_SIZE + i * RECORD_SIZE;
    const r = decodeRecord(buf.subarray(offset, offset + RECORD_SIZE));
    if (typeof r === 'string') return { ok: false, reason: r, badRecord: i };
    records.push(r);
  }
  return {
    ok: true,
    frame: { version: buf[0], frameFlags: buf[1], seqFirst: view.getUint32(4, true), records },
  };
}
