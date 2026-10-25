import { fileURLToPath } from 'node:url';
import { defineBackend } from '@aws-amplify/backend';
import { Duration } from 'aws-cdk-lib';
import { DynamoDbDataSource } from 'aws-cdk-lib/aws-appsync';
import { Runtime, StartingPosition } from 'aws-cdk-lib/aws-lambda';
import { DynamoEventSource } from 'aws-cdk-lib/aws-lambda-event-sources';
import { auth } from './auth/resource';
import { data } from './data/resource';
import { notifyReadings } from './data/notify/resource';
import { createIngestion } from './ingest/stack';
import { createStationIdentities, TOPIC_PREFIX } from './iot/resource';

const backend = defineBackend({ auth, data, notifyReadings });

/* Station CSRs live in the repository's gitignored certs/ directory. */
const csrDir = fileURLToPath(new URL('../../certs', import.meta.url));

const iotStack = backend.createStack('StationIdentity');
const stations = createStationIdentities(iotStack, csrDir);

const { table } = createIngestion(backend.createStack('Ingestion'));

/* Accounts are created by an administrator: nobody can sign themselves up to read the data. */
const { cfnUserPool, cfnIdentityPool } = backend.auth.resources.cfnResources;
cfnUserPool.adminCreateUserConfig = { allowAdminCreateUserOnly: true };
/* No anonymous AWS identities: the dashboard needs a signed-in user for every call. */
cfnIdentityPool.allowUnauthenticatedIdentities = false;

/*
 * The API reads the readings table through this data source. It is created here rather than with
 * backend.data.addDynamoDbDataSource, which always grants read and write; the dashboard only reads.
 */
const api = backend.data.resources.graphqlApi;
new DynamoDbDataSource(api, 'ReadingsTable', { api, table, name: 'ReadingsTable', readOnlyAccess: true });

/* defineFunction offers Node.js up to 22; run the same runtime as the ingest Lambda. */
backend.notifyReadings.resources.cfnResources.cfnFunction.runtime = Runtime.NODEJS_24_X.name;

/*
 * Every stored reading reaches the notify function through the table's stream. A push that keeps
 * failing is dropped after 2 retries or 60 s: the reading is stored, and the dashboard catches up
 * with a query, so an old live update is worth less than a blocked stream.
 */
backend.notifyReadings.resources.lambda.addEventSource(
  new DynamoEventSource(table, {
    startingPosition: StartingPosition.LATEST,
    batchSize: 100,
    retryAttempts: 2,
    maxRecordAge: Duration.seconds(60),
  }),
);

backend.addOutput({
  custom: {
    iot: {
      topicPrefix: TOPIC_PREFIX,
      stations,
    },
    readingsTable: table.tableName,
  },
});
