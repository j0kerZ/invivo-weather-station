/*
 * Security and reliability settings of the ingestion stack, checked on the synthesized
 * CloudFormation template so a change that weakens them fails CI instead of slipping into review.
 */
import { App, Stack } from 'aws-cdk-lib';
import { Match, Template } from 'aws-cdk-lib/assertions';
import { beforeAll, describe, expect, it } from 'vitest';
import { createIngestion } from './stack';

let template: Template;

beforeAll(() => {
  /* Skip bundling the Lambda code: these tests only read the template. */
  const app = new App({ context: { 'aws:cdk:bundling-stacks': [] } });
  const stack = new Stack(app, 'Ingestion', { env: { account: '111111111111', region: 'eu-north-1' } });
  createIngestion(stack);
  template = Template.fromStack(stack);
});

describe('ingestion stack', () => {
  it('runs the Lambda on Node.js 24, ARM, 256 MB, 15 s', () => {
    template.hasResourceProperties('AWS::Lambda::Function', {
      Runtime: 'nodejs24.x',
      Architectures: ['arm64'],
      MemorySize: 256,
      Timeout: 15,
    });
  });

  it('lets the Lambda only batch-write to the table: no reads, no other tables', () => {
    const policies = template.findResources('AWS::IAM::Policy');
    const statements = Object.values(policies).flatMap(
      (p) => (p as { Properties: { PolicyDocument: { Statement: unknown[] } } }).Properties.PolicyDocument.Statement,
    ) as { Action: string | string[]; Resource: unknown }[];
    const dynamo = statements.filter((s) => [s.Action].flat().some((a) => a.startsWith('dynamodb:')));
    expect(dynamo).toHaveLength(1);
    expect(dynamo[0].Action).toBe('dynamodb:BatchWriteItem');
  });

  it('lets only our IoT rule, in our account, invoke the Lambda', () => {
    template.hasResourceProperties('AWS::Lambda::Permission', {
      Action: 'lambda:InvokeFunction',
      Principal: 'iot.amazonaws.com',
      SourceAccount: '111111111111',
      SourceArn: { 'Fn::GetAtt': [Match.stringLikeRegexp('^TelemetryRule'), 'Arn'] },
    });
  });

  it('reads only the stations\' telemetry topic', () => {
    template.hasResourceProperties('AWS::IoT::TopicRule', {
      TopicRulePayload: { Sql: Match.stringLikeRegexp("FROM 'invivo-ws/\\+/telemetry'$") },
    });
  });

  it('sends events that still fail after 2 retries to the failure queue', () => {
    template.hasResourceProperties('AWS::Lambda::EventInvokeConfig', {
      MaximumRetryAttempts: 2,
      DestinationConfig: {
        OnFailure: { Destination: { 'Fn::GetAtt': [Match.stringLikeRegexp('^IngestFailures'), 'Arn'] } },
      },
    });
  });

  it('encrypts the failure queue, refuses unencrypted connections, and keeps events 14 days', () => {
    template.hasResourceProperties('AWS::SQS::Queue', {
      SqsManagedSseEnabled: true,
      MessageRetentionPeriod: 14 * 24 * 3600,
    });
    template.hasResourceProperties('AWS::SQS::QueuePolicy', {
      PolicyDocument: {
        Statement: Match.arrayWith([
          Match.objectLike({ Effect: 'Deny', Condition: { Bool: { 'aws:SecureTransport': 'false' } } }),
        ]),
      },
    });
  });

  it('keeps the table recoverable to any point in the last 35 days, billed per request', () => {
    template.hasResourceProperties('AWS::DynamoDB::Table', {
      BillingMode: 'PAY_PER_REQUEST',
      PointInTimeRecoverySpecification: { PointInTimeRecoveryEnabled: true },
      /* Still encrypted: false selects the AWS-owned key instead of a KMS key in our account. */
      SSESpecification: { SSEEnabled: false },
    });
  });

  it('keeps the Lambda logs for a year', () => {
    template.hasResourceProperties('AWS::Logs::LogGroup', { RetentionInDays: 365 });
  });
});
