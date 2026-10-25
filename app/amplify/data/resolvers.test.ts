/*
 * The AppSync resolvers, run in Node with a small stand-in for the AppSync runtime utilities: the
 * key range a query reads, the input it refuses, and who may publish.
 */
import { beforeEach, describe, expect, it, vi } from 'vitest';

const setSubscriptionFilter = vi.fn();

vi.mock('@aws-appsync/utils', () => {
  class ResolverError extends Error {
    constructor(message: string, readonly type?: string) { super(message); }
  }
  return {
    util: {
      matches: (pattern: string, value: string) => new RegExp(pattern).test(value),
      error: (message: string, type?: string) => { throw new ResolverError(message, type); },
      unauthorized: () => { throw new ResolverError('Unauthorized', 'Unauthorized'); },
      dynamodb: { toMapValues: (values: Record<string, unknown>) => values },
      transform: { toSubscriptionFilter: (filter: unknown) => ({ filter }) },
    },
    extensions: { setSubscriptionFilter },
  };
});

const listReadings = await import('./listReadings.js');
const publishReadings = await import('./publishReadings.js');
const onReadings = await import('./onReadings.js');

const query = (args: Record<string, unknown>) => listReadings.request({ args });

describe('listReadings', () => {
  const base = { stationId: 'station-001', kind: 'minute', fromTs: 1792742400, toTs: 1792742460 };

  it('reads one key range: the station, the kind prefix, and the time range inclusive', () => {
    expect(query(base)).toEqual({
      operation: 'Query',
      query: {
        expression: 'stationId = :s AND sk BETWEEN :from AND :to',
        expressionValues: { ':s': 'station-001', ':from': 'M#1792742400', ':to': 'M#1792742460~' },
      },
      scanIndexForward: true,
      limit: 500,
      nextToken: undefined,
    });
  });

  it('reads changes with the C# prefix, newest first, with a page size', () => {
    const req = query({ ...base, kind: 'change', newestFirst: true, limit: 10, nextToken: 'abc' });
    expect(req.query.expressionValues[':from']).toBe('C#1792742400');
    expect(req.scanIndexForward).toBe(false);
    expect(req.limit).toBe(10);
    expect(req.nextToken).toBe('abc');
  });

  it('includes every key at the end second: "~" sorts after "#", "." and digits', () => {
    const upper = query(base).query.expressionValues[':to'] as unknown as string; // stand-in keeps plain values
    expect('M#1792742460#0000000001' < upper).toBe(true);
    expect('M#1792742461#0000000000' > upper).toBe(true);
  });

  it.each([
    [{ stationId: '../x' }, 'Invalid stationId'],
    [{ kind: 'all' }, 'kind must be "minute" or "change"'],
    [{ fromTs: 10, toTs: 5 }, 'Invalid time range'],
    [{ toTs: 4294967296 }, 'Invalid time range'],
    [{ limit: 0 }, 'limit must be 1..1000'],
    [{ limit: 1001 }, 'limit must be 1..1000'],
  ])('refuses %o', (override, message) => {
    expect(() => query({ ...base, ...override })).toThrow(message);
  });

  it('returns the items and the next page token', () => {
    const page = listReadings.response({ result: { items: [{ sk: 'M#1' }], nextToken: 't' } });
    expect(page).toEqual({ items: [{ sk: 'M#1' }], nextToken: 't' });
  });
});

describe('publishReadings', () => {
  const args = { stationId: 'station-001', items: '[{"sk":"M#1"}]' };

  it('lets an IAM principal (the notify function) publish', () => {
    expect(publishReadings.request({ args, identity: { userArn: 'arn:aws:sts::1:assumed-role/notify/x' } })).toEqual({});
    expect(publishReadings.response({ args })).toEqual({ stationId: 'station-001', items: [{ sk: 'M#1' }] });
  });

  it.each([
    ['a signed-in user', { sub: 'abc', claims: {} }],
    ['no identity', undefined],
  ])('refuses %s', (_, identity) => {
    expect(() => publishReadings.request({ args, identity })).toThrow('Unauthorized');
  });
});

describe('onReadings', () => {
  beforeEach(() => setSubscriptionFilter.mockClear());

  it('filters events on the server to the station the client asked for', () => {
    expect(onReadings.response({ args: { stationId: 'station-002' } })).toBeNull();
    expect(setSubscriptionFilter).toHaveBeenCalledWith({ filter: { stationId: { eq: 'station-002' } } });
  });
});
