/*
 * The dashboard's only access to data: the GraphQL API, signed with the user's Cognito token.
 * The browser never holds AWS credentials and never talks to DynamoDB.
 */
import { generateClient } from 'aws-amplify/data';
import type { Schema } from '../amplify/data/resource';
import type { Reading } from './readings';

const client = generateClient<Schema>();

const PAGE_SIZE = 1000;

export interface ReadingQuery {
  stationId: string;
  kind: 'minute' | 'change';
  fromTs: number; // Unix seconds, inclusive
  toTs: number;
  newestFirst?: boolean;
  /** Stop after this many readings; all pages when omitted. */
  max?: number;
}

/** One key-range query, following the pages the API returns. */
export async function fetchReadings({ max, ...query }: ReadingQuery): Promise<Reading[]> {
  const readings: Reading[] = [];
  let nextToken: string | null | undefined;
  do {
    const limit = Math.min(PAGE_SIZE, max === undefined ? PAGE_SIZE : max - readings.length);
    const { data, errors } = await client.queries.listReadings({ ...query, limit, nextToken });
    if (errors?.length) throw new Error(errors.map((e) => e.message).join('; '));
    readings.push(...(data?.items ?? []));
    nextToken = data?.nextToken;
  } while (nextToken && (max === undefined || readings.length < max));
  return readings;
}

/** Live readings of one station, pushed as they are stored. Returns the unsubscribe function. */
export function subscribeReadings(
  stationId: string,
  onReadings: (readings: Reading[]) => void,
  onError: (error: Error) => void,
): () => void {
  const sub = client.subscriptions.onReadings({ stationId }).subscribe({
    next: (batch) => onReadings(batch.items),
    error: (e: unknown) => onError(new Error(describeError(e))),
  });
  return () => sub.unsubscribe();
}

/* Amplify reports subscription failures as { errors: [...] } objects, not Errors. */
export function describeError(e: unknown): string {
  if (e instanceof Error) return e.message;
  if (typeof e === 'object' && e !== null && 'errors' in e && Array.isArray(e.errors)) {
    return e.errors.map((x: { message?: string }) => x?.message ?? JSON.stringify(x)).join('; ');
  }
  return typeof e === 'string' ? e : JSON.stringify(e);
}
