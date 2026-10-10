import React from 'react';
import { NavLink, useNavigate } from 'react-router-dom';
import { MessageCircle, UserPlus, Radar, User } from 'lucide-react';
import { colors, motion, shadow } from '../theme/theme';

type Tab = { to: string; label: string; Icon: typeof MessageCircle; badge?: number };

const TABS: Tab[] = [
  { to: '/chats', label: 'Chats', Icon: MessageCircle },
  { to: '/requests', label: 'Requests', Icon: UserPlus, badge: 2 },
  { to: '/profile', label: 'Profile', Icon: User },
];

export default function BottomNav() {
  const navigate = useNavigate();
  return (
    <nav style={bottomNavStyles.root}>
      {TABS.slice(0, 2).map((t) => <TabItem key={t.to} {...t} />)}
      <span />
      <TabItem {...TABS[2]} />
      <button
        style={bottomNavStyles.fab}
        onClick={() => navigate('/nearby')}
        aria-label="Find people nearby"
      >
        <Radar size={25} strokeWidth={2} color={colors.white} />
        <span style={bottomNavStyles.fabLabel}>NEARBY</span>
      </button>
    </nav>
  );
}

function TabItem({ to, label, Icon, badge }: Tab) {
  return (
    <NavLink to={to} style={bottomNavStyles.tab}>
      {({ isActive }) => (
        <>
          <Icon size={22} strokeWidth={isActive ? 2.1 : 1.9} color={isActive ? colors.accentText : colors.textFaint} />
          <span style={{ ...bottomNavStyles.tabLabel, color: isActive ? colors.accentText : colors.textFaint, fontWeight: isActive ? 700 : 600 }}>{label}</span>
          {badge ? <span style={bottomNavStyles.badge}>{badge}</span> : null}
        </>
      )}
    </NavLink>
  );
}

const bottomNavStyles: Record<string, React.CSSProperties> = {
  root: { position: 'relative', flex: 'none', height: 92, paddingTop: 12, display: 'grid', gridTemplateColumns: 'repeat(4,1fr)', alignItems: 'start', borderTop: '1px solid ' + colors.border, background: colors.bgElevated },
  tab: { position: 'relative', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6, textDecoration: 'none', transition: 'color ' + motion.fast },
  tabLabel: { fontSize: 10.5 },
  badge: { position: 'absolute', top: -2, right: 24, minWidth: 17, height: 17, padding: '0 4px', borderRadius: 9, background: colors.accent, color: colors.white, fontSize: 10, fontWeight: 700, lineHeight: '17px', textAlign: 'center' },
  fab: { position: 'absolute', left: '62.5%', top: -26, transform: 'translateX(-50%)', width: 66, height: 66, border: 0, borderRadius: 24, background: colors.accent, boxShadow: shadow.fab, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 2, cursor: 'pointer', transition: 'transform ' + motion.base },
  fabLabel: { fontSize: 9, fontWeight: 700, color: 'rgba(255,255,255,0.9)' },
};
