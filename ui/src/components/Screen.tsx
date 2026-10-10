import React from 'react';
import { colors, font } from '../theme/theme';

/**
 * Full-window app shell: a flex column that exactly fills the WebView.
 *
 * Height is `100%` (html/body/#root are all 100% in index.html), NOT `100dvh`:
 * older Android System WebViews (still common on Android 11–13 devices that
 * have not updated it) do not understand `dvh`, drop the declaration, and the
 * shell collapses to its content height — leaving a blank band at the bottom
 * or pushing the footer CTA off-screen.
 *
 * The real OS status and navigation bars are drawn by Android, so the shell
 * only pads for the TOP safe-area inset (non-zero on edge-to-edge / notched
 * devices).
 *
 * There is deliberately NO paddingBottom here. Padding on this flex column sits
 * outside its children, so a bottom inset stopped BottomNav short of the screen
 * edge and left a strip of plain `bg` showing under the bar's `bgElevated` —
 * the empty band at the foot of every tabbed screen. Whatever occupies the
 * bottom of a screen owns that inset instead, so its own background runs all the
 * way down: see bottomNavStyles.root, and the footers in Welcome/SetupAccount.
 */
export default function Screen({ children }: { children: React.ReactNode }) {
  return <div style={screenStyles.root}>{children}</div>;
}

const screenStyles: Record<string, React.CSSProperties> = {
  root: {
    display: 'flex',
    flexDirection: 'column',
    height: '100%',
    boxSizing: 'border-box',
    paddingTop: 'env(safe-area-inset-top, 0px)',
    background: colors.bg,
    color: colors.text,
    fontFamily: font.family,
    overflow: 'hidden',
    position: 'relative',
  },
};
