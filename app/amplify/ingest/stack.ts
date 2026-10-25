/*
 * Ingestion: IoT rule -> Lambda -> DynamoDB, with a queue for events the Lambda could not store.
 */
import { fileURLToPath } from 'node:url';
import { Duration, RemovalPolicy, Stack } from 'aws-cdk-lib';
import * as dynamodb from 'aws-cdk-lib/aws-dynamodb';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as iot from 'aws-cdk-lib/aws-iot';
import * as lambda from 'aws-cdk-lib/aws-lambda';
import { SqsDestination } from 'aws-cdk-lib/aws-lambda-destinations';
import { NodejsFunction } from 'aws-cdk-lib/aws-lambda-nodejs';
import * as logs from 'aws-cdk-lib/aws-logs';
import * as sqs from 'aws-cdk-lib/aws-sqs';
import { TOPIC_PREFIX } from '../iot/resource';

export function createIngestion(stack: Stack) {
  /*
   * One item per record. Partition key: station. Sort key: "M#<ts>#<seq>" for minute summaries,
   * "C#<ts>.<tenths>" for changes, so each dashboard question is one range query on one prefix.
   * Removed with the environment: production would set RETAIN and deletion protection.
   */
  const table = new dynamodb.Table(stack, 'Readings', {
    partitionKey: { name: 'stationId', type: dynamodb.AttributeType.STRING },
    sortKey: { name: 'sk', type: dynamodb.AttributeType.STRING },
    billingMode: dynamodb.BillingMode.PAY_PER_REQUEST,
    encryption: dynamodb.TableEncryption.DEFAULT, // AWS-owned key: weather readings are not personal data
    pointInTimeRecoverySpecification: { pointInTimeRecoveryEnabled: true },
    stream: dynamodb.StreamViewType.NEW_IMAGE, // stored readings, for live dashboard updates
    removalPolicy: RemovalPolicy.DESTROY,
  });

  /*
   * IoT Core invokes the Lambda asynchronously, and acknowledges the station before the Lambda
   * runs, so the station never resends. An event that still fails after Lambda's retries is kept
   * here (14 days, the SQS maximum) instead of being discarded.
   */
  const failed = new sqs.Queue(stack, 'IngestFailures', {
    retentionPeriod: Duration.days(14),
    encryption: sqs.QueueEncryption.SQS_MANAGED,
    enforceSSL: true,
  });

  const ingest = new NodejsFunction(stack, 'Ingest', {
    entry: fileURLToPath(new URL('./handler.ts', import.meta.url)),
    runtime: lambda.Runtime.NODEJS_24_X,
    architecture: lambda.Architecture.ARM_64,
    memorySize: 256,
    timeout: Duration.seconds(15),
    environment: { TABLE_NAME: table.tableName },
    bundling: { externalModules: [] }, // bundle the AWS SDK instead of using the runtime's copy
    retryAttempts: 2,
    onFailure: new SqsDestination(failed),
    logGroup: new logs.LogGroup(stack, 'IngestLogs', {
      retention: logs.RetentionDays.ONE_YEAR,
      removalPolicy: RemovalPolicy.DESTROY,
    }),
  });
  table.grant(ingest, 'dynamodb:BatchWriteItem'); // the only call the Lambda makes: no reads

  /*
   * The rule passes the binary payload as base64, and the station ID from the topic. The topic
   * segment is trustworthy because the IoT policy only lets a station publish to its own topic.
   */
  const rule = new iot.CfnTopicRule(stack, 'TelemetryRule', {
    topicRulePayload: {
      sql: `SELECT encode(*, 'base64') AS payload, topic(2) AS stationId, timestamp() AS receivedAt FROM '${TOPIC_PREFIX}/+/telemetry'`,
      awsIotSqlVersion: '2016-03-23',
      actions: [{ lambda: { functionArn: ingest.functionArn } }],
    },
  });
  ingest.addPermission('InvokedByIotRule', {
    principal: new iam.ServicePrincipal('iot.amazonaws.com'),
    sourceArn: rule.attrArn,
    sourceAccount: stack.account,
  });

  return { table, ingest, failed };
}
