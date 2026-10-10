import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { LogoTile } from '../components/Logo';
import { colors, font } from '../theme/theme';
import { useBluetooth } from '../hooks/useBluetooth';
import { api } from '../api/client';

const MIN_SHOW_MS = 1400;

/**
 * Entry route. Shows the brand while it works out where to send the user:
 *   no local account  -> /welcome
 *   account, BT off   -> /bluetooth
 *   account, BT on    -> /nearby
 */
export default function SplashScreen() {
  const navigate = useNavigate();
  const { state: btState } = useBluetooth();
  const [hasAccount, setHasAccount] = useState<boolean | null>(null);
  const [minElapsed, setMinElapsed] = useState(false);
  const [progress, setProgress] = useState(0.15);

  useEffect(() => {
    api.getAccount().then(() => setHasAccount(true)).catch(() => setHasAccount(false));
    const t = window.setTimeout(() => setMinElapsed(true), MIN_SHOW_MS);
    const p = window.setTimeout(() => setProgress(0.7), 80);
    return () => { clearTimeout(t); clearTimeout(p); };
  }, []);

  useEffect(() => {
    if (!minElapsed || hasAccount === null) return;
    if (!hasAccount) { setProgress(1); navigate('/welcome', { replace: true }); return; }
    if (btState === 'unknown') return;
    setProgress(1);
    navigate(btState === 'on' ? '/nearby' : '/bluetooth', { replace: true });
  }, [minElapsed, hasAccount, btState, navigate]);

  return (
    <div style={splashStyles.root}>
      <div style={splashStyles.glow} />
      <div style={splashStyles.center}>
        <div style={splashStyles.markWrap}>
          <span style={splashStyles.ring} />
          <span style={{ ...splashStyles.ring, animationDelay: '1.4s' }} />
          <LogoTile size={104} style={splashStyles.tileIn} />
        </div>
        <div style={splashStyles.words}>
          <span style={splashStyles.name}>AnyTime</span>
          <span style={splashStyles.tagline}>Chat with people around you</span>
        </div>
      </div>
      <div style={splashStyles.footer}>
        <span style={splashStyles.track}>
          <span style={{ ...splashStyles.bar, transform: 'scaleX(' + progress + ')' }} />
        </span>
        <span style={splashStyles.caption}>WORKS WITHOUT INTERNET</span>
      </div>
    </div>
  );
}

const splashStyles: Record<string, React.CSSProperties> = {
  root: { position: 'relative', height: '100dvh', display: 'flex', flexDirection: 'column', alignItems: 'center', background: colors.bg, color: colors.text, fontFamily: font.family, overflow: 'hidden' },
  glow: { position: 'absolute', top: '50%', left: '50%', transform: 'translate(-50%,-62%)', width: 560, height: 560, borderRadius: '50%', background: colors.glow, pointerEvents: 'none' },
  center: { position: 'relative', flex: 1, marginTop: -40, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 26 },
  markWrap: { position: 'relative', width: 104, height: 104, display: 'flex', alignItems: 'center', justifyContent: 'center' },
  ring: { position: 'absolute', width: 220, height: 220, borderRadius: '50%', border: '1.5px solid ' + colors.accentBorder, animation: 'at-ringPulse 2.8s ease-out infinite' },
  tileIn: { animation: 'at-logoIn 0.6s cubic-bezier(0.22,0.61,0.36,1) both' },
  words: { display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8, animation: 'at-softIn 0.6s ease 0.2s both' },
  name: { fontSize: 34, fontWeight: 800, letterSpacing: '-0.035em' },
  tagline: { fontSize: 14.5, fontWeight: 500, color: colors.textMuted },
  footer: { position: 'relative', paddingBottom: 44, display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 14, animation: 'at-softIn 0.6s ease 0.4s both' },
  track: { width: 120, height: 3, borderRadius: 2, background: colors.border, overflow: 'hidden', display: 'block' },
  bar: { display: 'block', width: '100%', height: '100%', borderRadius: 2, background: colors.accent, transformOrigin: 'left', transition: 'transform 900ms cubic-bezier(0.22,0.61,0.36,1)' },
  caption: { fontSize: 11.5, fontWeight: 600, letterSpacing: '0.04em', color: colors.textFaint },
};
