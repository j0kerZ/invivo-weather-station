/*
 * Dashboard sign-in: passwordless, with a one-time code by email or a passkey (WebAuthn).
 *
 * Cognito sends the codes through Amazon SES, from an address that SES has already verified
 * (see README). The address comes from the environment so it is not committed.
 */
import { defineAuth } from '@aws-amplify/backend';

const senderEmail = process.env.WS_SENDER_EMAIL;
if (!senderEmail) {
  throw new Error('Set WS_SENDER_EMAIL to an SES-verified address before deploying (see README)');
}

export const auth = defineAuth({
  loginWith: {
    email: { otpLogin: true },
    webAuthn: true, // relying party: localhost in a sandbox, the app domain in a branch deployment
  },
  senders: {
    email: { fromEmail: senderEmail, fromName: 'InVivo Weather Station' },
  },
});
