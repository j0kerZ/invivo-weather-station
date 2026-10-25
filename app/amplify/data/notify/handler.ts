/*
 * Notify Lambda: receives new and changed items from the readings table's DynamoDB stream and
 * publishes them per station through the publishReadings mutation, which AppSync turns into
 * onReadings events for the dashboard. Signs its calls with its own IAM role.
 */
import { getAmplifyDataClientConfig } from '@aws-amplify/backend/function/runtime';
import { Amplify } from 'aws-amplify';
import { generateClient } from 'aws-amplify/data';
import type { Schema } from '../resource';
import { makeHandler } from './stream';

type DataClientEnv = Parameters<typeof getAmplifyDataClientConfig>[0];

let client: ReturnType<typeof generateClient<Schema>> | undefined;

async function publishReadings(stationId: string, items: Record<string, unknown>[]): Promise<void> {
  if (!client) {
    const { resourceConfig, libraryOptions } = await getAmplifyDataClientConfig(process.env as DataClientEnv);
    Amplify.configure(resourceConfig, libraryOptions);
    client = generateClient<Schema>({ authMode: 'iam' });
  }
  const { errors } = await client.mutations.publishReadings({ stationId, items: JSON.stringify(items) });
  /* Throwing makes Lambda retry the stream batch; the readings are already stored either way. */
  if (errors?.length) throw new Error(`publishReadings failed: ${errors.map((e) => e.message).join('; ')}`);
}

export const handler = makeHandler(publishReadings);
