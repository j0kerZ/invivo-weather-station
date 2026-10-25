import { defineFunction } from '@aws-amplify/backend';

/*
 * Pushes stored readings to dashboard subscribers. Grouped with the API ("data") so its access to
 * the API and its trigger on the readings table only reference stacks in one direction.
 */
export const notifyReadings = defineFunction({
  name: 'notify-readings',
  entry: './handler.ts',
  resourceGroupName: 'data',
  architecture: 'arm64',
  memoryMB: 256,
  timeoutSeconds: 15,
  logging: { format: 'json', retention: '1 year' },
});
