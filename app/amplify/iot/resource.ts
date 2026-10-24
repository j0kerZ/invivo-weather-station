/*
 * Device identity on AWS IoT Core: one thing and one X.509 certificate per station, and a policy
 * that lets a station publish only to its own topic.
 *
 * Certificates are created from certificate signing requests (CSRs). The station generates its key
 * pair and sends only the CSR, so the private key never leaves the device. On production hardware
 * the key would be generated inside a secure element; here tools/station-credentials.sh generates
 * it on the developer's machine, into the gitignored certs/ directory.
 */
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { Stack } from 'aws-cdk-lib';
import * as iot from 'aws-cdk-lib/aws-iot';

/** MQTT topics are `invivo-ws/{stationId}/telemetry` (docs/PROTOCOL.md §6). */
export const TOPIC_PREFIX = 'invivo-ws';

export interface StationIdentity {
  stationId: string;
  certificateId: string;
}

/** Thing names become topic segments and policy variables, so keep them simple. */
const STATION_ID = /^[a-z0-9-]{3,64}$/;

export function createStationIdentities(stack: Stack, csrDir: string): StationIdentity[] {
  const arn = (resource: string) => `arn:aws:iot:${stack.region}:${stack.account}:${resource}`;

  /*
   * Policy variables make one policy serve every station: ${iot:Connection.Thing.ThingName} is
   * the thing the connecting certificate is attached to. A station can only connect with its own
   * thing name as client ID, and only publish to its own topic. Publishing anywhere else makes
   * IoT Core close the connection.
   */
  const policy = new iot.CfnPolicy(stack, 'StationPolicy', {
    policyDocument: {
      Version: '2012-10-17',
      Statement: [
        {
          Effect: 'Allow',
          Action: 'iot:Connect',
          Resource: arn('client/${iot:Connection.Thing.ThingName}'),
          Condition: { Bool: { 'iot:Connection.Thing.IsAttached': 'true' } },
        },
        {
          Effect: 'Allow',
          Action: 'iot:Publish',
          Resource: arn(`topic/${TOPIC_PREFIX}/\${iot:Connection.Thing.ThingName}/telemetry`),
        },
      ],
    },
  });

  const csrFiles = existsSync(csrDir) ? readdirSync(csrDir).filter((f) => f.endsWith('.csr')) : [];
  return csrFiles.sort().map((file) => {
    const stationId = file.slice(0, -'.csr'.length);
    if (!STATION_ID.test(stationId)) {
      throw new Error(`Invalid station ID "${stationId}" (from ${file}): use [a-z0-9-], 3-64 chars`);
    }

    const thing = new iot.CfnThing(stack, `Thing-${stationId}`, { thingName: stationId });
    const cert = new iot.CfnCertificate(stack, `Certificate-${stationId}`, {
      certificateSigningRequest: readFileSync(join(csrDir, file), 'utf8'),
      status: 'ACTIVE',
    });
    new iot.CfnThingPrincipalAttachment(stack, `ThingCertificate-${stationId}`, {
      thingName: thing.ref,
      principal: cert.attrArn,
    });
    new iot.CfnPolicyPrincipalAttachment(stack, `PolicyCertificate-${stationId}`, {
      policyName: policy.ref,
      principal: cert.attrArn,
    });

    return { stationId, certificateId: cert.ref };
  });
}
