import React, { useEffect, useState } from 'react';
import { Bluetooth, Lock, ShieldCheck, ChevronRight, Sun, Moon, Monitor } from 'lucide-react';
import Screen from '../components/Screen';
import BottomNav from '../components/BottomNav';
import { colors, motion, shadow, applyThemeMode, getThemeMode, ThemeMode } from '../theme/theme';
import { api } from '../api/client';

const MODES: Array<{ id: ThemeMode; label: string; Icon: typeof Sun }> = [
  { id: 'light', label: 'Light', Icon: Sun },
  { id: 'dark', label: 'Dark', Icon: Moon },
  { id: 'system', label: 'System', Icon: Monitor },
];

export default function ProfileScreen() {
  const [me, setMe] = useState({ username: 'vinay', age: 25, gender: 'Male', discoverable: true, acceptFrom: 'Everyone nearby' });
  const [mode, setMode] = useState<ThemeMode>(getThemeMode());

  useEffect(() => {
    api.getAccount().then((a) => setMe((m) => ({ ...m, username: a.username, gender: a.gender, discoverable: a.discoverable, acceptFrom: a.acceptFrom, age: yearsSince(a.dob) }))).catch(() => {});
  }, []);

  function pickMode(next: ThemeMode) {
    setMode(next);
    applyThemeMode(next);
  }

  function toggleDiscoverable() {
    const next = !me.discoverable;
    setMe((m) => ({ ...m, discoverable: next }));
    api.updateAccount({ discoverable: next }).catch(() => setMe((m) => ({ ...m, discoverable: !next })));
  }

  return (
    <Screen>
      <div style={profileStyles.glow} />
      <div style={profileStyles.head}>
        <span style={profileStyles.avatar}>{me.username.charAt(0).toUpperCase()}</span>
        <h2 style={profileStyles.name}>@{me.username}</h2>
        <p style={profileStyles.meta}>{me.age} · {me.gender} · On this device</p>
        <button style={profileStyles.edit}>Edit profile</button>
      </div>

      <div style={profileStyles.list}>
        <div style={profileStyles.row}>
          <Bluetooth size={19} strokeWidth={2} color={colors.accent} />
          <span style={profileStyles.rowText}>
            <span style={profileStyles.rowTitle}>Discoverable nearby</span>
            <span style={profileStyles.rowSub}>Others can see @{me.username} over Bluetooth</span>
          </span>
          <button style={{ ...profileStyles.toggle, background: me.discoverable ? colors.accent : colors.borderStrong, justifyContent: me.discoverable ? 'flex-end' : 'flex-start' }} onClick={toggleDiscoverable} aria-pressed={me.discoverable}>
            <span style={profileStyles.knob} />
          </button>
        </div>

        <div style={profileStyles.appearance}>
          <span style={profileStyles.appearanceHead}>
            <Sun size={19} strokeWidth={2} color={colors.text60} />
            <span style={profileStyles.rowText}>
              <span style={profileStyles.rowTitle}>Appearance</span>
              <span style={profileStyles.rowSub}>Light is the default</span>
            </span>
          </span>
          <div style={profileStyles.segmented}>
            {MODES.map(({ id, label, Icon }) => (
              <button key={id} onClick={() => pickMode(id)} style={{ ...profileStyles.segment, ...(mode === id ? profileStyles.segmentActive : null) }}>
                <Icon size={14} strokeWidth={2.1} />{label}
              </button>
            ))}
          </div>
        </div>

        <div style={profileStyles.row}>
          <Lock size={19} strokeWidth={2} color={colors.text60} />
          <span style={profileStyles.rowText}>
            <span style={profileStyles.rowTitle}>Accept messages from</span>
            <span style={profileStyles.rowSub}>{me.acceptFrom}</span>
          </span>
          <ChevronRight size={16} strokeWidth={2.2} color={colors.text35} />
        </div>

        <div style={profileStyles.row}>
          <ShieldCheck size={19} strokeWidth={2} color={colors.text60} />
          <span style={profileStyles.rowText}>
            <span style={profileStyles.rowTitle}>Blocked users</span>
            <span style={profileStyles.rowSub}>None</span>
          </span>
          <ChevronRight size={16} strokeWidth={2.2} color={colors.text35} />
        </div>
      </div>

      <BottomNav />
    </Screen>
  );
}

function yearsSince(dob: string): number {
  const d = new Date(dob);
  const now = new Date();
  let a = now.getFullYear() - d.getFullYear();
  const m = now.getMonth() - d.getMonth();
  if (m < 0 || (m === 0 && now.getDate() < d.getDate())) a--;
  return a;
}

const profileStyles: Record<string, React.CSSProperties> = {
  glow: { position: 'absolute', top: -170, left: '50%', transform: 'translateX(-50%)', width: 460, height: 460, borderRadius: '50%', background: colors.glow, pointerEvents: 'none' },
  head: { position: 'relative', padding: '24px 24px 0', display: 'flex', flexDirection: 'column', alignItems: 'center', textAlign: 'center', flex: 'none' },
  avatar: { width: 84, height: 84, borderRadius: 28, background: colors.accent, color: colors.white, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 30, fontWeight: 800, boxShadow: shadow.accentLg },
  name: { margin: '14px 0 0', fontSize: 22, fontWeight: 800, letterSpacing: '-0.02em' },
  meta: { margin: '6px 0 0', fontSize: 13, fontWeight: 500, color: colors.text50 },
  edit: { marginTop: 14, height: 42, padding: '0 20px', borderRadius: 14, border: '1px solid ' + colors.borderStrong, background: colors.surface, color: colors.text, fontSize: 13, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer', transition: 'background ' + motion.fast },
  list: { flex: 1, overflowY: 'auto', padding: '22px 20px 0', display: 'flex', flexDirection: 'column', gap: 10 },
  row: { display: 'flex', alignItems: 'center', gap: 13, padding: '15px 16px', borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border },
  rowText: { flex: 1, display: 'flex', flexDirection: 'column', gap: 2, minWidth: 0 },
  rowTitle: { fontSize: 14, fontWeight: 700 },
  rowSub: { fontSize: 11.5, fontWeight: 500, color: colors.text50 },
  toggle: { width: 46, height: 27, borderRadius: 14, border: 0, padding: 3, display: 'flex', alignItems: 'center', flex: 'none', cursor: 'pointer', transition: 'background ' + motion.base },
  knob: { width: 21, height: 21, borderRadius: '50%', background: colors.white, display: 'block' },
  appearance: { padding: '15px 16px', borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border, display: 'flex', flexDirection: 'column', gap: 13 },
  appearanceHead: { display: 'flex', alignItems: 'center', gap: 13 },
  segmented: { display: 'flex', gap: 6, padding: 4, borderRadius: 14, background: colors.fillSoft },
  segment: { flex: 1, height: 38, borderRadius: 11, border: 0, background: 'transparent', color: colors.text50, display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 7, fontSize: 12.5, fontWeight: 600, fontFamily: 'inherit', whiteSpace: 'nowrap', cursor: 'pointer', transition: 'background ' + motion.fast + ', color ' + motion.fast },
  segmentActive: { background: colors.surface, color: colors.text, fontWeight: 700, boxShadow: '0 1px 3px rgba(18,18,26,0.12)' },
};
