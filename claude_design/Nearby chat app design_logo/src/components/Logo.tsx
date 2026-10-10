import React from 'react';
import { colors, font } from '../theme/theme';

/**
 * AnyTime mark — "Ping bubble": a chat bubble carrying the same signal arcs as
 * the Nearby button. Drawn inline (lucide has no brand marks).
 *  - LogoMark: bare mark, `tone` = fill of the bubble
 *  - LogoTile: app-icon tile (accent square, white bubble)
 *  - Wordmark: mark + "AnyTime" lockup
 */
const BUBBLE = 'M20 8h24a12 12 0 0 1 12 12v18a12 12 0 0 1-12 12H24l-10 8v-9.2A12 12 0 0 1 8 38V20A12 12 0 0 1 20 8z';
const ARCS = 'M25 26a9 9 0 0 1 9 9M25 19a16 16 0 0 1 16 16';

export function LogoMark({ size = 32, tone = colors.accent, ink = colors.white }: { size?: number; tone?: string; ink?: string }) {
  return (
    <svg width={size} height={size} viewBox="0 0 64 64" aria-hidden="true" style={logoStyles.svg}>
      <path d={BUBBLE} fill={tone} />
      <circle cx="25" cy="35" r="3.6" fill={ink} />
      <path d={ARCS} fill="none" stroke={ink} strokeWidth={4.2} strokeLinecap="round" />
    </svg>
  );
}

export function LogoTile({ size = 104, style }: { size?: number; style?: React.CSSProperties }) {
  return (
    <span style={{ ...logoStyles.tile, width: size, height: size, borderRadius: size * 0.29, ...style }}>
      <LogoMark size={size * 0.62} tone={colors.white} ink={colors.accent} />
    </span>
  );
}

export function Wordmark({ size = 26 }: { size?: number }) {
  return (
    <span style={logoStyles.lockup}>
      <LogoMark size={size * 1.3} />
      <span style={{ ...logoStyles.word, fontSize: size }}>AnyTime</span>
    </span>
  );
}

const logoStyles: Record<string, React.CSSProperties> = {
  svg: { display: 'block', flex: 'none' },
  tile: { position: 'relative', display: 'flex', alignItems: 'center', justifyContent: 'center', background: colors.accent, boxShadow: '0 16px 40px rgba(68,83,246,0.35)', flex: 'none' },
  lockup: { display: 'inline-flex', alignItems: 'center', gap: 11 },
  word: { fontFamily: font.family, fontWeight: 800, letterSpacing: '-0.035em', color: colors.text },
};
