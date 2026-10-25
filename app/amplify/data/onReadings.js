/*
 * onReadings: the server filters events, so a subscriber receives only the station it asked for.
 */
import { extensions, util } from '@aws-appsync/utils';

export function request() {
  return { payload: null };
}

export function response(ctx) {
  extensions.setSubscriptionFilter(
    util.transform.toSubscriptionFilter({ stationId: { eq: ctx.args.stationId } }),
  );
  return null;
}
