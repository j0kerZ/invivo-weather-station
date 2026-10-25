/*
 * The notify Lambda's logic, without AWS clients: turn DynamoDB stream records into one publish
 * call per station.
 */
import type { AttributeValue } from '@aws-sdk/client-dynamodb';
import { unmarshall } from '@aws-sdk/util-dynamodb';
import type { DynamoDBStreamEvent } from 'aws-lambda';

export type Publish = (stationId: string, items: Record<string, unknown>[]) => Promise<void>;

/** New and changed items, grouped by station. Deletions are not readings to show. */
export function groupByStation(event: DynamoDBStreamEvent): Map<string, Record<string, unknown>[]> {
  const byStation = new Map<string, Record<string, unknown>[]>();
  for (const record of event.Records) {
    const image = record.dynamodb?.NewImage;
    if (record.eventName === 'REMOVE' || !image) continue;
    const item = unmarshall(image as Record<string, AttributeValue>);
    const list = byStation.get(item.stationId as string) ?? [];
    list.push(item);
    byStation.set(item.stationId as string, list);
  }
  return byStation;
}

export function makeHandler(publish: Publish) {
  return async (event: DynamoDBStreamEvent): Promise<void> => {
    for (const [stationId, items] of groupByStation(event)) {
      await publish(stationId, items);
    }
  };
}
