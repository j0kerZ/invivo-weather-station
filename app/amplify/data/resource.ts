/*
 * Dashboard API (AppSync GraphQL). Signed-in users can read readings and receive live updates;
 * nobody can write through this API. Readings live in the ingestion table (ingest/stack.ts), which
 * the API reads through a read-only data source named ReadingsTable (backend.ts).
 *
 * Live updates: AppSync pushes to subscribers when a mutation runs, and readings are written by
 * the ingest Lambda, not through this API. So the notify function calls publishReadings for each
 * stored batch; that mutation stores nothing and is callable only by that function.
 */
import { type ClientSchema, a, defineData } from '@aws-amplify/backend';
import { notifyReadings } from './notify/resource';

const schema = a.schema({
  /** One stored record; field meanings as in the README's Readings table. */
  Reading: a.customType({
    stationId: a.string().required(),
    sk: a.string().required(),
    kind: a.string().required(), // "minute" | "change"
    ts: a.timestamp().required(), // Unix seconds: u32, beyond GraphQL Int (signed 32-bit)
    tenths: a.integer().required(),
    seq: a.float(), // u32, beyond GraphQL Int; exact as a double
    backfill: a.boolean(),
    tempMean: a.float(),
    tempMin: a.float(),
    tempMax: a.float(),
    rhMean: a.float(),
    rhMin: a.float(),
    rhMax: a.float(),
    tempValidPct: a.integer().required(),
    rhValidPct: a.integer().required(),
    wetBulb: a.float(),
    flags: a.string().array().required(),
    receivedAt: a.float().required(),
  }),

  ReadingPage: a.customType({
    items: a.ref('Reading').required().array().required(),
    nextToken: a.string(),
  }),

  ReadingBatch: a.customType({
    stationId: a.string().required(),
    items: a.ref('Reading').required().array().required(),
  }),

  /** A station's minute summaries or changes with fromTs <= ts <= toTs (Unix seconds). */
  listReadings: a
    .query()
    .arguments({
      stationId: a.string().required(),
      kind: a.string().required(),
      fromTs: a.timestamp().required(),
      toTs: a.timestamp().required(),
      newestFirst: a.boolean(),
      limit: a.integer(),
      nextToken: a.string(),
    })
    .returns(a.ref('ReadingPage').required())
    .authorization((allow) => [allow.authenticated()])
    .handler(a.handler.custom({ dataSource: 'ReadingsTable', entry: './listReadings.js' })),

  publishReadings: a
    .mutation()
    .arguments({
      stationId: a.string().required(),
      items: a.json().required(),
    })
    .returns(a.ref('ReadingBatch').required())
    /*
     * No user may call this: "SystemOnly" is a group nobody is added to. The notify function gets
     * in through IAM (schema rule below), which Amplify authorizes by IAM policy, not these rules.
     */
    .authorization((allow) => [allow.group('SystemOnly')])
    .handler(a.handler.custom({ entry: './publishReadings.js' })),

  /** Live readings for one station. */
  onReadings: a
    .subscription()
    .for(a.ref('publishReadings'))
    .arguments({ stationId: a.string().required() })
    .authorization((allow) => [allow.authenticated()])
    .handler(a.handler.custom({ entry: './onReadings.js' })),
})
  /* The notify function may run mutations (only publishReadings exists) and nothing else. */
  .authorization((allow) => [allow.resource(notifyReadings).to(['mutate'])]);

export type Schema = ClientSchema<typeof schema>;

export const data = defineData({
  schema,
  authorizationModes: { defaultAuthorizationMode: 'userPool' },
});
