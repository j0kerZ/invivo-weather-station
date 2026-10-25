/*
 * publishReadings: stores nothing (no data source). Its only effect is the onReadings event that
 * AppSync sends to subscribers of the station.
 */
import { util } from '@aws-appsync/utils';

export function request(ctx) {
  /* Only an IAM principal (the notify function) may publish; signed-in users never can. */
  if (!ctx.identity || !ctx.identity.userArn) util.unauthorized();
  return {};
}

export function response(ctx) {
  const { stationId, items } = ctx.args;
  return { stationId, items: typeof items === 'string' ? JSON.parse(items) : items };
}
