import { marshall } from '@aws-sdk/util-dynamodb';
import type { DynamoDBRecord, DynamoDBStreamEvent } from 'aws-lambda';
import { describe, expect, it } from 'vitest';
import { groupByStation, makeHandler } from './stream';

const record = (eventName: DynamoDBRecord['eventName'], item?: Record<string, unknown>): DynamoDBRecord => ({
  eventName,
  dynamodb: item ? { NewImage: marshall(item) as NonNullable<DynamoDBRecord['dynamodb']>['NewImage'] } : {},
});

const minute = (stationId: string, seq: number) => ({ stationId, sk: `M#1792742400#${String(seq).padStart(10, '0')}`, seq, tempMean: 1.5 });

describe('groupByStation', () => {
  it('groups new and changed readings by station, in stream order', () => {
    const event: DynamoDBStreamEvent = {
      Records: [
        record('INSERT', minute('station-001', 1)),
        record('INSERT', minute('station-002', 7)),
        record('MODIFY', minute('station-001', 2)),
      ],
    };
    const groups = groupByStation(event);
    expect([...groups.keys()]).toEqual(['station-001', 'station-002']);
    expect(groups.get('station-001')!.map((i) => i.seq)).toEqual([1, 2]);
    expect(groups.get('station-002')![0]).toEqual(minute('station-002', 7));
  });

  it('ignores deletions and records without a new image', () => {
    const event: DynamoDBStreamEvent = {
      Records: [record('REMOVE', minute('station-001', 1)), record('INSERT')],
    };
    expect(groupByStation(event).size).toBe(0);
  });
});

describe('makeHandler', () => {
  it('publishes once per station', async () => {
    const calls: [string, number][] = [];
    const handler = makeHandler(async (stationId, items) => { calls.push([stationId, items.length]); });
    await handler({
      Records: [
        record('INSERT', minute('station-001', 1)),
        record('INSERT', minute('station-001', 2)),
        record('INSERT', minute('station-002', 1)),
      ],
    });
    expect(calls).toEqual([['station-001', 2], ['station-002', 1]]);
  });

  it('fails when a publish fails, so Lambda retries the batch', async () => {
    const handler = makeHandler(async () => { throw new Error('AppSync down'); });
    await expect(handler({ Records: [record('INSERT', minute('station-001', 1))] })).rejects.toThrow('AppSync down');
  });
});
