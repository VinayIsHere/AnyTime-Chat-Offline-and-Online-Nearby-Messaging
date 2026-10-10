import React from 'react';
import { colors, font } from '../theme/theme';

/** Phone-shaped app shell: status bar + content column. */
export default function Screen({ children }: { children: React.ReactNode }) {
  return (
    <div style={screenStyles.root}>
      <div style={screenStyles.statusBar}>
        <span>9:41</span>
        <span style={screenStyles.statusRight}>
          <span style={{ fontSize: 10.5, letterSpacing: '0.06em' }}>5G</span>
          <span style={screenStyles.battery}><span style={screenStyles.batteryFill} /></span>
        </span>
      </div>
      {children}
    </div>
  );
}

const screenStyles: Record<string, React.CSSProperties> = {
  root: { display: 'flex', flexDirection: 'column', height: '100dvh', background: colors.bg, color: colors.text, fontFamily: font.family, overflow: 'hidden', position: 'relative' },
  statusBar: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '16px 26px 0', fontSize: 12.5, fontWeight: 600, color: colors.text85, flex: 'none' },
  statusRight: { display: 'flex', gap: 6, alignItems: 'center' },
  battery: { width: 22, height: 11, border: '1px solid rgba(245,245,247,0.5)', borderRadius: 3, padding: 1.5, display: 'block' },
  batteryFill: { display: 'block', width: '70%', height: '100%', background: colors.text, borderRadius: 1 },
};
