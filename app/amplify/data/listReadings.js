/*
 * listReadings: one DynamoDB Query on the station's partition, over one sort-key prefix
 * ("M#" minute summaries, "C#" changes), so the time range maps to a key range.
 */
import { util } from '@aws-appsync/utils';

const DEFAULT_LIMIT = 500;
const MAX_LIMIT = 1000;
const MAX_TS = 4294967295; // u32 Unix seconds
const STATION_ID = '^[a-z0-9-]{3,64}$';

const pad10 = (n) => `0000000000${n}`.slice(-10);

export function request(ctx) {
  const { stationId, kind, fromTs, toTs, newestFirst, limit, nextToken } = ctx.args;
  if (!util.matches(STATION_ID, stationId)) util.error('Invalid stationId', 'BadRequest');
  if (kind !== 'minute' && kind !== 'change') util.error('kind must be "minute" or "change"', 'BadRequest');
  if (fromTs < 0 || toTs < fromTs || toTs > MAX_TS) util.error('Invalid time range', 'BadRequest');
  if (limit !== undefined && limit !== null && (limit < 1 || limit > MAX_LIMIT)) {
    util.error(`limit must be 1..${MAX_LIMIT}`, 'BadRequest');
  }

  /* "~" sorts after "#", "." and every digit, so the upper bound includes every key at toTs. */
  const prefix = kind === 'minute' ? 'M#' : 'C#';
  return {
    operation: 'Query',
    query: {
      expression: 'stationId = :s AND sk BETWEEN :from AND :to',
      expressionValues: util.dynamodb.toMapValues({
        ':s': stationId,
        ':from': `${prefix}${pad10(fromTs)}`,
        ':to': `${prefix}${pad10(toTs)}~`,
      }),
    },
    scanIndexForward: !newestFirst,
    limit: limit || DEFAULT_LIMIT,
    nextToken,
  };
}

export function response(ctx) {
  if (ctx.error) util.error(ctx.error.message, ctx.error.type);
  return { items: ctx.result.items, nextToken: ctx.result.nextToken };
}
