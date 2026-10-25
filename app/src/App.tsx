/*
 * Sign-in (passwordless: email code or passkey; accounts are created by an administrator), then
 * the overview of all stations or one station's page. The page lives in the URL hash, so the
 * browser's back button and links work.
 */
import { Authenticator } from '@aws-amplify/ui-react';
import '@aws-amplify/ui-react/styles.css';
import { useEffect, useState } from 'react';
import { FarmOverview } from './components/FarmOverview';
import { StationDetail } from './components/StationDetail';
import { chooseRange, type Range } from './range';
import { STATIONS } from './stations';

const stationFromHash = (): string | undefined => {
  const id = decodeURIComponent(window.location.hash.replace(/^#\/station\//, ''));
  return STATIONS.includes(id) ? id : undefined;
};

function Dashboard() {
  const [stationId, setStationId] = useState(stationFromHash);
  const [range, setRange] = useState<Range>(() => chooseRange('24 h'));
  useEffect(() => {
    const onHash = () => setStationId(stationFromHash());
    window.addEventListener('hashchange', onHash);
    return () => window.removeEventListener('hashchange', onHash);
  }, []);

  return stationId ? (
    <StationDetail stationId={stationId} range={range} onRange={(name) => setRange(chooseRange(name))} onBack={() => { window.location.hash = ''; }} />
  ) : (
    <FarmOverview range={range} onRange={(name) => setRange(chooseRange(name))} onOpen={(id) => { window.location.hash = `/station/${encodeURIComponent(id)}`; }} />
  );
}

export default function App() {
  return (
    <Authenticator
      hideSignUp
      passwordless={{ preferredAuthMethod: 'EMAIL_OTP', hiddenAuthMethods: ['PASSWORD'] }}
      components={{ SignIn: { Footer: () => null } }} // no "Forgot your password?": users have none
    >
      {({ signOut, user }) => (
        <main>
          <header className="top">
            <h1>Farm weather</h1>
            <span className="muted">{user?.signInDetails?.loginId}</span>
            <button type="button" onClick={signOut}>Sign out</button>
          </header>
          <Dashboard />
        </main>
      )}
    </Authenticator>
  );
}
