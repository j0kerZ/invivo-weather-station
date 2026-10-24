/*
 * The decoder against the vectors the C code generates (firmware/tools/gen_vectors.c): station and
 * cloud must accept the same frames, decode the same values and reject with the same reason.
 */
import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { crc16, crc8, decodeFrame } from './protocol';

interface Vector {
  name: string;
  expect: string;
  badRecord?: number;
  frame?: unknown;
  hex: string;
}

const vectors: Vector[] = JSON.parse(
  readFileSync(new URL('../../../protocol/vectors-v1.json', import.meta.url), 'utf8'),
).vectors;

const fromHex = (hex: string) => new Uint8Array(Buffer.from(hex, 'hex'));

describe('shared test vectors', () => {
  it('covers every vector the C generator writes', () => {
    expect(vectors.length).toBe(24);
  });

  it.each(vectors)('$name: $expect', (v) => {
    const result = decodeFrame(fromHex(v.hex));
    if (v.expect === 'OK') {
      expect(result).toEqual({ ok: true, frame: v.frame });
    } else {
      expect(result).toEqual({
        ok: false,
        reason: v.expect,
        ...(v.badRecord === undefined ? {} : { badRecord: v.badRecord }),
      });
    }
  });
});

describe('checksums', () => {
  /* The standard check values for "123456789" (PROTOCOL.md §9). */
  const check = new TextEncoder().encode('123456789');

  it('CRC-8/NRSC-5', () => expect(crc8(check)).toBe(0xf7));
  it('CRC-16/CCITT-FALSE', () => expect(crc16(check)).toBe(0x29b1));
});
