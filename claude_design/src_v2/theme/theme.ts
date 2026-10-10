/**
 * All theme values are CSS custom properties so inline styles stay static
 * Record<string, React.CSSProperties> objects while the theme can still switch
 * at runtime. Light is the default; dark is opted into in Profile > Appearance.
 */
export const colors = {
  bg: 'var(--at-bg)',
  bgElevated: 'var(--at-bg-elev)',
  surface: 'var(--at-surface)',
  surfaceHover: 'var(--at-surface-hover)',
  bubble: 'var(--at-bubble)',
  avatar: 'var(--at-avatar)',
  border: 'var(--at-line)',
  borderStrong: 'var(--at-line-2)',
  fillSoft: 'var(--at-fill-soft)',
  fillSofter: 'var(--at-fill-softer)',
  text: 'var(--at-text)',
  text85: 'var(--at-text-85)',
  text70: 'var(--at-text-70)',
  text60: 'var(--at-text-60)',
  textMuted: 'var(--at-text-55)',
  text50: 'var(--at-text-50)',
  text45: 'var(--at-text-45)',
  textFaint: 'var(--at-text-40)',
  text35: 'var(--at-text-35)',
  accent: '#4453F6',
  accentHover: '#3543E6',
  accentSoft: 'var(--at-accent-soft)',
  accentBorder: 'var(--at-accent-border)',
  accentText: 'var(--at-accent-text)',
  success: 'var(--at-success)',
  offline: 'var(--at-offline)',
  danger: 'var(--at-danger)',
  ring: 'var(--at-ring)',
  fade: 'var(--at-fade)',
  glow: 'var(--at-glow)',
  white: '#FFFFFF',
} as const;

export const font = {
  family: 'Manrope, system-ui, -apple-system, sans-serif',
  h1: { fontSize: 38, fontWeight: 800, lineHeight: 1.08, letterSpacing: '-0.03em' },
  h2: { fontSize: 27, fontWeight: 800, lineHeight: 1.15, letterSpacing: '-0.02em' },
  h3: { fontSize: 22, fontWeight: 800, letterSpacing: '-0.02em' },
  body: { fontSize: 15, fontWeight: 500, lineHeight: 1.55 },
  label: { fontSize: 12, fontWeight: 600, letterSpacing: '0.02em' },
  caption: { fontSize: 11.5, fontWeight: 600 },
} as const;

export const radius = { sm: 8, md: 12, lg: 14, xl: 18, xxl: 24, pill: 999 } as const;
export const space = { xs: 6, sm: 10, md: 16, lg: 24, xl: 32 } as const;

export const shadow = {
  card: '0 2px 8px rgba(18,18,26,0.04)',
  accent: '0 12px 30px rgba(68,83,246,0.28)',
  accentLg: '0 14px 34px rgba(68,83,246,0.30)',
  fab: '0 0 0 6px var(--at-ring), 0 14px 30px rgba(68,83,246,0.38)',
} as const;

export const motion = {
  fast: '160ms cubic-bezier(0.22,0.61,0.36,1)',
  base: '260ms cubic-bezier(0.22,0.61,0.36,1)',
  slow: '420ms cubic-bezier(0.22,0.61,0.36,1)',
} as const;

const LIGHT = [
  '--at-bg:#F7F7FA',
  '--at-bg-elev:rgba(255,255,255,0.97)',
  '--at-surface:#FFFFFF',
  '--at-surface-hover:rgba(18,18,26,0.035)',
  '--at-bubble:#FFFFFF',
  '--at-avatar:#EDEDF3',
  '--at-line:rgba(18,18,26,0.08)',
  '--at-line-2:rgba(18,18,26,0.14)',
  '--at-fill-soft:rgba(18,18,26,0.05)',
  '--at-fill-softer:rgba(18,18,26,0.03)',
  '--at-text:#12121A',
  '--at-text-85:rgba(18,18,26,0.85)',
  '--at-text-70:rgba(18,18,26,0.70)',
  '--at-text-60:rgba(18,18,26,0.60)',
  '--at-text-55:rgba(18,18,26,0.55)',
  '--at-text-50:rgba(18,18,26,0.50)',
  '--at-text-45:rgba(18,18,26,0.45)',
  '--at-text-40:rgba(18,18,26,0.40)',
  '--at-text-35:rgba(18,18,26,0.35)',
  '--at-accent-soft:rgba(68,83,246,0.10)',
  '--at-accent-border:rgba(68,83,246,0.22)',
  '--at-accent-text:#3543E6',
  '--at-success:#0E9E82',
  '--at-offline:#A8A8B4',
  '--at-danger:#D93B3B',
  '--at-ring:#F7F7FA',
  '--at-fade:linear-gradient(rgba(247,247,250,0),#F7F7FA 40%)',
  '--at-glow:radial-gradient(circle,rgba(68,83,246,0.18),rgba(68,83,246,0) 64%)',
].join(';');

const DARK = [
  '--at-bg:#0B0B0F',
  '--at-bg-elev:rgba(15,15,21,0.96)',
  '--at-surface:rgba(255,255,255,0.045)',
  '--at-surface-hover:rgba(255,255,255,0.09)',
  '--at-bubble:#1A1A24',
  '--at-avatar:#232330',
  '--at-line:rgba(255,255,255,0.09)',
  '--at-line-2:rgba(255,255,255,0.14)',
  '--at-fill-soft:rgba(255,255,255,0.05)',
  '--at-fill-softer:rgba(255,255,255,0.035)',
  '--at-text:#F5F5F7',
  '--at-text-85:rgba(245,245,247,0.85)',
  '--at-text-70:rgba(245,245,247,0.70)',
  '--at-text-60:rgba(245,245,247,0.60)',
  '--at-text-55:rgba(245,245,247,0.55)',
  '--at-text-50:rgba(245,245,247,0.50)',
  '--at-text-45:rgba(245,245,247,0.45)',
  '--at-text-40:rgba(245,245,247,0.40)',
  '--at-text-35:rgba(245,245,247,0.35)',
  '--at-accent-soft:rgba(68,83,246,0.14)',
  '--at-accent-border:rgba(68,83,246,0.30)',
  '--at-accent-text:#8E9BFF',
  '--at-success:#12C2A0',
  '--at-offline:#5A5A66',
  '--at-danger:#FF6B6B',
  '--at-ring:#0B0B0F',
  '--at-fade:linear-gradient(rgba(11,11,15,0),#0B0B0F 40%)',
  '--at-glow:radial-gradient(circle,rgba(68,83,246,0.32),rgba(68,83,246,0) 62%)',
].join(';');

const SHEET = [
  ':root{' + LIGHT + ';color-scheme:light}',
  '[data-theme="dark"]{' + DARK + ';color-scheme:dark}',
  '@keyframes at-ringPulse{0%{transform:scale(.35);opacity:.85}100%{transform:scale(1.45);opacity:0}}',
  '@keyframes at-sweep{to{transform:rotate(360deg)}}',
  '@keyframes at-softIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}',
  '@keyframes at-blink{0%,49%{opacity:1}50%,100%{opacity:0}}',
  '@keyframes at-spin{to{transform:rotate(360deg)}}',
].join('');

/** Injects the token + keyframe stylesheet once. Called from App. */
export function installTheme(): void {
  if (typeof document === 'undefined' || document.getElementById('at-theme')) return;
  const el = document.createElement('style');
  el.id = 'at-theme';
  el.textContent = SHEET;
  document.head.appendChild(el);
}

export type ThemeMode = 'light' | 'dark' | 'system';
const STORAGE_KEY = 'anytime.theme';

export function getThemeMode(): ThemeMode {
  const saved = typeof localStorage === 'undefined' ? null : localStorage.getItem(STORAGE_KEY);
  return saved === 'dark' || saved === 'light' || saved === 'system' ? saved : 'light';
}

export function applyThemeMode(mode: ThemeMode): void {
  installTheme();
  const dark = mode === 'dark' || (mode === 'system' && window.matchMedia('(prefers-color-scheme: dark)').matches);
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  localStorage.setItem(STORAGE_KEY, mode);
}
