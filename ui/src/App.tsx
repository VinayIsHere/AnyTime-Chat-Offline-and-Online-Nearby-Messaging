import { useEffect, useState } from 'react';
import { BrowserRouter, Navigate, Route, Routes } from 'react-router-dom';

import { ROUTER_BASENAME } from './api/base';
import { ApiError, api, type Account } from './api/client';
import { applyThemeMode, colors, font, getThemeMode } from './theme/theme';

import ChatScreen from './screens/ChatScreen';
import ChatsScreen from './screens/ChatsScreen';
import NearbyScreen from './screens/NearbyScreen';
import ProfileScreen from './screens/ProfileScreen';
import RequestsScreen from './screens/RequestsScreen';
import SetupAccountScreen from './screens/SetupAccountScreen';
import WelcomeScreen from './screens/WelcomeScreen';

type Boot =
  | { phase: 'loading' }
  | { phase: 'no-account' }
  | { phase: 'ready'; account: Account }
  | { phase: 'unreachable' };

export default function App() {
  const [boot, setBoot] = useState<Boot>({ phase: 'loading' });

  useEffect(() => {
    applyThemeMode(getThemeMode());
  }, []);

  // One account probe decides the entry route: /welcome before setup,
  // /nearby after. 404 is the expected "not set up yet" answer, not a failure.
  useEffect(() => {
    let cancelled = false;

    api
      .getAccount()
      .then((account) => {
        if (!cancelled) setBoot({ phase: 'ready', account });
      })
      .catch((err: unknown) => {
        if (cancelled) return;
        if (err instanceof ApiError && err.status === 404) setBoot({ phase: 'no-account' });
        else setBoot({ phase: 'unreachable' });
      });

    return () => {
      cancelled = true;
    };
  }, []);

  if (boot.phase === 'loading') return <Splash />;
  if (boot.phase === 'unreachable') return <CoreUnreachable />;

  const entry = boot.phase === 'ready' ? '/nearby' : '/welcome';

  return (
    <BrowserRouter basename={ROUTER_BASENAME}>
      <Routes>
        <Route path="/" element={<Navigate to={entry} replace />} />
        <Route path="/welcome" element={<WelcomeScreen />} />
        <Route path="/setup" element={<SetupAccountScreen />} />
        <Route path="/nearby" element={<NearbyScreen />} />
        <Route path="/requests" element={<RequestsScreen />} />
        <Route path="/chats" element={<ChatsScreen />} />
        <Route path="/chats/:chatId" element={<ChatScreen />} />
        <Route path="/profile" element={<ProfileScreen />} />
        <Route path="*" element={<Navigate to={entry} replace />} />
      </Routes>
    </BrowserRouter>
  );
}

/** Shown for the few hundred ms the account probe takes. */
function Splash() {
  return (
    <div style={bootStyles.root}>
      <div style={bootStyles.mark} />
    </div>
  );
}

/**
 * The core is not answering on loopback. In the shipped app this means the
 * C++ shell failed to start its server, which the user cannot fix — so the copy
 * says what happened rather than asking them to do something impossible.
 */
function CoreUnreachable() {
  return (
    <div style={bootStyles.root}>
      <div style={bootStyles.panel}>
        <h1 style={bootStyles.title}>AnyTime could not start</h1>
        <p style={bootStyles.body}>
          The app&rsquo;s local service is not responding. Closing AnyTime completely and opening
          it again usually clears this.
        </p>
        <button style={bootStyles.retry} onClick={() => window.location.reload()}>
          Try again
        </button>
      </div>
    </div>
  );
}

const bootStyles: Record<string, React.CSSProperties> = {
  root: {
    height: '100dvh',
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    background: colors.bg,
    color: colors.text,
    fontFamily: font.family,
    padding: 28,
  },
  mark: {
    width: 56,
    height: 56,
    borderRadius: 18,
    background: colors.accent,
    opacity: 0.9,
  },
  panel: { maxWidth: 320, textAlign: 'center' },
  title: { margin: 0, fontSize: 22, fontWeight: 800, letterSpacing: '-0.02em' },
  body: {
    margin: '12px 0 22px',
    fontSize: 14,
    fontWeight: 500,
    lineHeight: 1.55,
    color: colors.textMuted,
  },
  retry: {
    height: 48,
    padding: '0 26px',
    border: 0,
    borderRadius: 14,
    background: colors.accent,
    color: colors.white,
    fontSize: 14.5,
    fontWeight: 700,
    fontFamily: 'inherit',
    cursor: 'pointer',
  },
};
