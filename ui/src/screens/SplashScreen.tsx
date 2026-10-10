import React, { useEffect, useState } from 'react';
import { LogoTile } from '../components/Logo';
import { colors, font } from '../theme/theme';

/**
 * How long the splash stays up when the app is opened, even if the core
 * answers at once — which on a phone it does, in a few milliseconds. Without a
 * floor the brand would flash past faster than it can be read.
 */
export const SPLASH_MIN_SHOW_MS = 1400;

/**
 * The brand, shown while App works out where to send the user.
 *
 * PRESENTATION ONLY. The handed-over design made this the `/` route and had it
 * probe the account and the radio itself. Here App already owns that probe —
 * it has to, because the router's entry route and the "could not start" screen
 * both depend on the answer — so this draws and App decides when it goes away.
 *
 * THE TILE'S POSITION IS SHARED WITH ANDROID. The native launch splash
 * (platform/android/res/drawable/splash.xml) draws the same tile and hands over
 * to this screen, so it is offset to land where this layout puts it. Moving the
 * tile here means moving it there.
 */
export default function SplashScreen() {
  const [progress, setProgress] = useState(0.15);

  // The bar is a sign of life, not a measurement: nothing here takes long
  // enough to report on. It eases most of the way and App replaces the screen.
  useEffect(() => {
    const t = window.setTimeout(() => setProgress(0.7), 80);
    return () => window.clearTimeout(t);
  }, []);

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
  // 100%, not 100dvh — old Android WebViews ignore dvh (see Screen.tsx).
  root: { position: 'relative', height: '100%', boxSizing: 'border-box', paddingTop: 'env(safe-area-inset-top, 0px)', display: 'flex', flexDirection: 'column', alignItems: 'center', background: colors.bg, color: colors.text, fontFamily: font.family, overflow: 'hidden' },
  glow: { position: 'absolute', top: '50%', left: '50%', transform: 'translate(-50%,-62%)', width: 560, height: 560, borderRadius: '50%', background: colors.glow, pointerEvents: 'none' },
  center: { position: 'relative', flex: 1, marginTop: -40, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 26 },
  markWrap: { position: 'relative', width: 104, height: 104, display: 'flex', alignItems: 'center', justifyContent: 'center' },
  ring: { position: 'absolute', width: 220, height: 220, borderRadius: '50%', border: '1.5px solid ' + colors.accentBorder, animation: 'at-ringPulse 2.8s ease-out infinite' },
  tileIn: { animation: 'at-logoIn 0.6s cubic-bezier(0.22,0.61,0.36,1) both' },
  words: { display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8, animation: 'at-softIn 0.6s ease 0.2s both' },
  name: { fontSize: 34, fontWeight: 800, letterSpacing: '-0.035em' },
  tagline: { fontSize: 14.5, fontWeight: 500, color: colors.textMuted },
  // Owns the bottom safe-area inset itself, like every other screen's footer.
  footer: { position: 'relative', paddingBottom: 'calc(44px + env(safe-area-inset-bottom, 0px))', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 14, animation: 'at-softIn 0.6s ease 0.4s both' },
  track: { width: 120, height: 3, borderRadius: 2, background: colors.border, overflow: 'hidden', display: 'block' },
  bar: { display: 'block', width: '100%', height: '100%', borderRadius: 2, background: colors.accent, transformOrigin: 'left', transition: 'transform 900ms cubic-bezier(0.22,0.61,0.36,1)' },
  caption: { fontSize: 11.5, fontWeight: 600, letterSpacing: '0.04em', color: colors.textFaint },
};
