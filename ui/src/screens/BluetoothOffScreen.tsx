import React, { useEffect } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import { Bluetooth, BluetoothOff, RefreshCw, Settings } from 'lucide-react';

import Screen from '../components/Screen';
import { colors, motion, shadow } from '../theme/theme';
import { useBluetooth } from '../hooks/useBluetooth';

const BLOCKED = [
  'Finding people nearby',
  'Sending and receiving messages',
  'Staying discoverable to others',
];

/**
 * Hard gate. AnyTime is Bluetooth-only in v1, so nothing else is reachable
 * until the radio is on — there is no dismiss and no bottom nav here.
 */
export default function BluetoothOffScreen() {
  const { state, requesting, denied, requestEnable, openSettings, canPromptInApp, isReady } =
    useBluetooth();
  const navigate = useNavigate();
  const location = useLocation();

  const unauthorized = state === 'unauthorized';

  // THE SCREEN CLOSES ITSELF, and it has to do it from here.
  //
  // RequireBluetooth guards the app's routes, but this route is deliberately
  // not guarded — so once the user has landed here, nothing else is watching.
  // Without this effect the radio could come back and the gate would simply
  // stay up, which is the one failure that makes the whole screen feel broken.
  useEffect(() => {
    if (!isReady) return;
    const from = (location.state as { from?: string } | null)?.from;
    navigate(from && from !== '/bluetooth' ? from : '/nearby', { replace: true });
  }, [isReady, navigate, location.state]);

  return (
    <Screen>
      <div style={bluetoothOffStyles.glow} />
      <div style={bluetoothOffStyles.body}>
        <div style={bluetoothOffStyles.markWrap}>
          <span style={bluetoothOffStyles.ring} />
          <span style={bluetoothOffStyles.ringWide} />
          <span style={bluetoothOffStyles.mark}>
            <BluetoothOff size={34} strokeWidth={1.9} color={colors.white} />
          </span>
        </div>

        <h1 style={bluetoothOffStyles.title}>
          {unauthorized ? 'AnyTime needs Bluetooth access' : 'Bluetooth is off'}
        </h1>
        <p style={bluetoothOffStyles.sub}>
          {unauthorized
            ? 'Allow the Bluetooth permission so AnyTime can find people around you. Nothing leaves your device.'
            : 'AnyTime works entirely over Bluetooth. Turn it on to find people around you and pick up your chats.'}
        </p>

        <div style={bluetoothOffStyles.card}>
          <span style={bluetoothOffStyles.cardLabel}>PAUSED UNTIL BLUETOOTH IS ON</span>
          {BLOCKED.map((b) => (
            <span key={b} style={bluetoothOffStyles.blockedRow}>
              <span style={bluetoothOffStyles.dot} />
              {b}
            </span>
          ))}
        </div>

        {denied && (
          <p style={bluetoothOffStyles.denied}>
            Bluetooth stayed off. You can turn it on from Settings instead.
          </p>
        )}
      </div>

      <div style={bluetoothOffStyles.footer}>
        <button style={bluetoothOffStyles.cta} onClick={requestEnable} disabled={requesting}>
          {requesting ? (
            <>
              <RefreshCw size={18} strokeWidth={2.2} style={bluetoothOffStyles.spin} />
              Waiting…
            </>
          ) : (
            <>
              <Bluetooth size={18} strokeWidth={2.2} />
              {canPromptInApp ? 'Turn on Bluetooth' : 'Open Bluetooth settings'}
            </>
          )}
        </button>
        {canPromptInApp && (
          <button style={bluetoothOffStyles.ghost} onClick={openSettings}>
            <Settings size={16} strokeWidth={2.1} />
            Open settings instead
          </button>
        )}
        <p style={bluetoothOffStyles.note}>
          {canPromptInApp
            ? 'Android will ask you to confirm. AnyTime never connects to the internet.'
            : 'Return to AnyTime after switching Bluetooth on — this screen closes itself.'}
        </p>
      </div>
    </Screen>
  );
}

const bluetoothOffStyles: Record<string, React.CSSProperties> = {
  glow: { position: 'absolute', top: -140, left: '50%', transform: 'translateX(-50%)', width: 520, height: 520, borderRadius: '50%', background: colors.glow, pointerEvents: 'none' },
  body: { position: 'relative', flex: 1, display: 'flex', flexDirection: 'column', padding: '0 28px', overflowY: 'auto' },
  markWrap: { marginTop: 92, position: 'relative', width: 76, height: 76, flex: 'none', display: 'flex', alignItems: 'center', justifyContent: 'center' },
  ring: { position: 'absolute', width: 132, height: 132, borderRadius: '50%', border: '1px dashed ' + colors.borderStrong },
  ringWide: { position: 'absolute', width: 196, height: 196, borderRadius: '50%', border: '1px dashed ' + colors.border },
  mark: { position: 'relative', width: 76, height: 76, borderRadius: 24, background: colors.accent, display: 'flex', alignItems: 'center', justifyContent: 'center', boxShadow: shadow.accentLg },
  title: { margin: '38px 0 0', fontSize: 30, fontWeight: 800, lineHeight: 1.12, letterSpacing: '-0.025em' },
  sub: { margin: '12px 0 0', fontSize: 15, fontWeight: 500, lineHeight: 1.55, color: colors.textMuted, maxWidth: 300 },
  card: { marginTop: 26, padding: '16px 18px', borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border, display: 'flex', flexDirection: 'column', gap: 11 },
  cardLabel: { fontSize: 10.5, fontWeight: 700, letterSpacing: '0.06em', color: colors.textFaint },
  blockedRow: { display: 'flex', alignItems: 'center', gap: 11, fontSize: 13.5, fontWeight: 600, color: colors.text70 },
  dot: { width: 6, height: 6, borderRadius: '50%', background: colors.offline, flex: 'none', display: 'block' },
  denied: { margin: '18px 0 0', fontSize: 12.5, fontWeight: 600, lineHeight: 1.5, color: colors.danger },
  // The footer owns the bottom safe-area inset, so its background runs to the
  // screen edge — see the note in Screen.tsx about why padding does not.
  footer: { flex: 'none', padding: '16px 28px 30px', paddingBottom: 'calc(30px + env(safe-area-inset-bottom, 0px))', display: 'flex', flexDirection: 'column', gap: 10, background: colors.fade },
  cta: { height: 56, border: 0, borderRadius: 16, background: colors.accent, color: colors.white, fontSize: 15.5, fontWeight: 700, fontFamily: 'inherit', display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 10, cursor: 'pointer', boxShadow: shadow.accent, transition: 'background ' + motion.fast },
  ghost: { height: 50, border: '1px solid ' + colors.borderStrong, borderRadius: 16, background: colors.surface, color: colors.text, fontSize: 14, fontWeight: 700, fontFamily: 'inherit', display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 9, cursor: 'pointer', transition: 'background ' + motion.fast },
  note: { margin: '6px 2px 0', fontSize: 11.5, fontWeight: 500, lineHeight: 1.5, color: colors.textFaint, textAlign: 'center' },
  // at-sweep, not at-spin: the theme defines at-ringPulse/at-sweep/at-softIn/
  // at-blink and nothing called at-spin, so the design's name would have left
  // this icon frozen while it claimed to be waiting.
  spin: { animation: 'at-sweep 1s linear infinite' },
};
