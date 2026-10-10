import { useEffect, useLayoutEffect, useState, type ReactElement } from 'react';
import { BrowserRouter, Navigate, Route, Routes, useLocation } from 'react-router-dom';

import { ROUTER_BASENAME } from './api/base';
import { ApiError, api, type Account } from './api/client';
import { applyThemeMode, colors, font, getThemeMode } from './theme/theme';
import { useBluetooth } from './hooks/useBluetooth';

import BluetoothOffScreen from './screens/BluetoothOffScreen';
import ChatScreen from './screens/ChatScreen';
import ChatsScreen from './screens/ChatsScreen';
import NearbyScreen from './screens/NearbyScreen';
import ProfileScreen from './screens/ProfileScreen';
import RequestsScreen from './screens/RequestsScreen';
import SetupAccountScreen from './screens/SetupAccountScreen';
import SplashScreen, { SPLASH_MIN_SHOW_MS } from './screens/SplashScreen';
import WelcomeScreen from './screens/WelcomeScreen';

type Boot =
  | { phase: 'loading' }
  | { phase: 'no-account' }
  | { phase: 'ready'; account: Account }
  | { phase: 'unreachable' };

/**
 * Whether this page load is the app being opened, rather than a route being
 * loaded directly.
 *
 * The shell opens the app at the root. Everything else that loads a page lands
 * deeper: tapping a notification makes AnyTimeActivity load `/chats/<id>`
 * outright, and a developer reloads whatever screen they are on. Those must not
 * sit behind a 1.4 s splash — someone tapping a message wants the message.
 */
function openedAtRoot(): boolean {
  const path = window.location.pathname;
  const route = ROUTER_BASENAME === '/' ? path : path.slice(ROUTER_BASENAME.length);
  return route === '' || route === '/';
}

export default function App() {
  const [boot, setBoot] = useState<Boot>({ phase: 'loading' });
  // Holds the splash for its minimum time on a real launch. Starts false on a
  // deep link, where the splash only covers the account probe itself.
  const [holdingSplash, setHoldingSplash] = useState(openedAtRoot);

  useEffect(() => {
    if (!holdingSplash) return;
    const t = window.setTimeout(() => setHoldingSplash(false), SPLASH_MIN_SHOW_MS);
    return () => window.clearTimeout(t);
  }, [holdingSplash]);

  // A LAYOUT effect, so the theme's variables and keyframes exist before the
  // first paint. As a plain effect the splash painted one frame without them —
  // no rings, default text colour — right as it took over from the native
  // launch splash, which is the one moment a flicker is most visible.
  useLayoutEffect(() => {
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

  if (boot.phase === 'loading' || holdingSplash) return <SplashScreen />;
  if (boot.phase === 'unreachable') return <CoreUnreachable />;

  const entry = boot.phase === 'ready' ? '/nearby' : '/welcome';

  return (
    <BrowserRouter basename={ROUTER_BASENAME}>
      <Routes>
        <Route path="/" element={<Navigate to={entry} replace />} />
        {/* Setup runs before the radio matters: someone with Bluetooth off can
            still create an account, and gating this would strand them with no
            way forward. */}
        <Route path="/welcome" element={<WelcomeScreen />} />
        <Route path="/setup" element={<SetupAccountScreen />} />

        {/* Deliberately NOT wrapped — wrapping the gate in the gate would loop.
            BluetoothOffScreen navigates away itself once the radio is back. */}
        <Route path="/bluetooth" element={<BluetoothOffScreen />} />

        <Route path="/nearby" element={<RequireBluetooth><NearbyScreen /></RequireBluetooth>} />
        <Route path="/requests" element={<RequireBluetooth><RequestsScreen /></RequireBluetooth>} />
        <Route path="/chats" element={<RequireBluetooth><ChatsScreen /></RequireBluetooth>} />
        <Route path="/chats/:chatId" element={<RequireBluetooth><ChatScreen /></RequireBluetooth>} />
        <Route path="/profile" element={<RequireBluetooth><ProfileScreen /></RequireBluetooth>} />
        <Route path="*" element={<Navigate to={entry} replace />} />
      </Routes>
    </BrowserRouter>
  );
}

/**
 * v1 is Bluetooth-only, so with the radio off every screen behind this is a
 * dead end: no peers, no sending, no receiving. It redirects rather than
 * overlaying a dismissible banner, because dismissing it would only reveal an
 * app that silently does nothing.
 *
 * `unsupported` passes straight through — that is desktop and browser
 * development, where there is no radio to gate on at all.
 */
function RequireBluetooth({ children }: { children: ReactElement }) {
  const { state, isReady } = useBluetooth();
  const location = useLocation();

  // One frame of nothing while the first status lands. Rendering the gate here
  // would flash it at everyone whose Bluetooth is perfectly fine.
  if (state === 'unknown') return null;

  // `from` is what lets the gate put the user back where they were rather than
  // dropping everyone on /nearby.
  if (!isReady) {
    return <Navigate to="/bluetooth" replace state={{ from: location.pathname }} />;
  }
  return children;
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
    // 100%, not 100dvh — old Android WebViews ignore dvh (see Screen.tsx).
    height: '100%',
    boxSizing: 'border-box',
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    background: colors.bg,
    color: colors.text,
    fontFamily: font.family,
    padding: 28,
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
