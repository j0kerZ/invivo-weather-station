import { fileURLToPath } from 'node:url';
import { defineBackend } from '@aws-amplify/backend';
import { createStationIdentities, TOPIC_PREFIX } from './iot/resource';

const backend = defineBackend({});

/* Station CSRs live in the repository's gitignored certs/ directory. */
const csrDir = fileURLToPath(new URL('../../certs', import.meta.url));

const iotStack = backend.createStack('StationIdentity');
const stations = createStationIdentities(iotStack, csrDir);

backend.addOutput({
  custom: {
    iot: {
      topicPrefix: TOPIC_PREFIX,
      stations,
    },
  },
});
