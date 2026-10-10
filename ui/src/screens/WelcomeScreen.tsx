import React from 'react';
import { useNavigate } from 'react-router-dom';
import { Check } from 'lucide-react';
import { LogoTile } from '../components/Logo';
import Screen from '../components/Screen';
import { colors, motion, shadow } from '../theme/theme';

const PROMISES = ['No phone number, no email', 'Your account stays on this device'];

export default function WelcomeScreen() {
  const navigate = useNavigate();
  return (
    <Screen>
      <div style={welcomeStyles.glow} />
      <div style={welcomeStyles.body}>
        <LogoTile size={76} style={welcomeStyles.mark} />
        <h1 style={welcomeStyles.title}>Chat with people<br />around you.</h1>
        <p style={welcomeStyles.sub}>Works with no internet. Pick a username, stay on Bluetooth, and start talking to people in range.</p>

        <div style={welcomeStyles.promises}>
          {PROMISES.map((p) => (
            <span key={p} style={welcomeStyles.promise}>
              <Check size={16} strokeWidth={2.4} color={colors.accent} />{p}
            </span>
          ))}
        </div>

        <div style={welcomeStyles.actions}>
          <button style={welcomeStyles.cta} onClick={() => navigate('/setup')}>Set up username</button>
          <p style={welcomeStyles.legal}>By continuing you agree to our Terms. AnyTime is for ages 13 and up.</p>
        </div>
      </div>
    </Screen>
  );
}

const welcomeStyles: Record<string, React.CSSProperties> = {
  glow: { position: 'absolute', top: -140, left: '50%', transform: 'translateX(-50%)', width: 520, height: 520, borderRadius: '50%', background: colors.glow, pointerEvents: 'none' },
  // Scrolls on short screens instead of letting the CTA collide with the promises.
  body: { position: 'relative', flex: 1, minHeight: 0, overflowY: 'auto', overflowX: 'hidden', display: 'flex', flexDirection: 'column', padding: '0 24px' },
  // Size, shape and colour come from LogoTile; this only places it.
  mark: { marginTop: 'clamp(32px, 10vh, 96px)', boxShadow: shadow.accentLg },
  title: { margin: '28px 0 0', fontSize: 'clamp(30px, 9vw, 38px)', fontWeight: 800, lineHeight: 1.08, letterSpacing: '-0.03em', textWrap: 'pretty' },
  sub: { margin: '14px 0 0', fontSize: 15, fontWeight: 500, lineHeight: 1.55, color: colors.textMuted, maxWidth: 296 },
  promises: { marginTop: 34, display: 'flex', flexDirection: 'column', gap: 12 },
  promise: { display: 'flex', alignItems: 'center', gap: 11, fontSize: 13.5, fontWeight: 600, color: colors.text70 },
  // Owns the bottom safe-area inset itself — Screen no longer pads for it.
  actions: { marginTop: 'auto', display: 'flex', flexDirection: 'column', gap: 12, paddingTop: 32, paddingBottom: 'calc(24px + env(safe-area-inset-bottom, 0px))', flex: 'none' },
  cta: { height: 56, border: 0, borderRadius: 16, background: colors.accent, color: colors.white, fontSize: 15.5, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer', boxShadow: shadow.accent, transition: 'background ' + motion.fast },
  legal: { margin: '8px 2px 0', fontSize: 11.5, fontWeight: 500, lineHeight: 1.5, color: colors.textFaint, textAlign: 'center' },
};
