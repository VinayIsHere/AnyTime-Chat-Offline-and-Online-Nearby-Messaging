import React, { useEffect, useState } from 'react';
import { NavLink, useNavigate } from 'react-router-dom';
import { MessageCircle, UserPlus, Radar, User } from 'lucide-react';
import { colors, motion, shadow } from '../theme/theme';
import { api } from '../api/client';
import { onEvent } from '../api/events';

type Tab = { to: string; label: string; Icon: typeof MessageCircle; badge?: number };

/**
 * @param requestCount  Pending-request count supplied by a screen that already
 *   holds the list (RequestsScreen), so its badge updates the instant a request
 *   is accepted or declined. Every other screen omits it and BottomNav tracks
 *   the count itself.
 */
export default function BottomNav({ requestCount }: { requestCount?: number }) {
  const navigate = useNavigate();
  const ownCount = usePendingRequestCount(requestCount === undefined);
  const badge = requestCount ?? ownCount;

  return (
    <nav style={bottomNavStyles.root}>
      <TabItem to="/chats" label="Chats" Icon={MessageCircle} />
      <TabItem to="/requests" label="Requests" Icon={UserPlus} badge={badge} />
      <span />
      <TabItem to="/profile" label="Profile" Icon={User} />
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

/** Live count of inbound requests: one fetch, then kept current over SSE. */
function usePendingRequestCount(enabled: boolean): number {
  const [ids, setIds] = useState<Set<string>>(() => new Set());

  useEffect(() => {
    if (!enabled) return;
    let cancelled = false;

    api.getRequests()
      .then((list) => {
        if (!cancelled) setIds(new Set(list.map((r) => r.id)));
      })
      .catch(() => {
        /* no badge beats a wrong badge */
      });

    const without = (id: string) =>
      setIds((current) => {
        if (!current.has(id)) return current;
        const next = new Set(current);
        next.delete(id);
        return next;
      });

    const offReceived = onEvent('request.received', (req) =>
      setIds((current) => (current.has(req.id) ? current : new Set(current).add(req.id))),
    );
    const offAccepted = onEvent('request.accepted', ({ request }) => without(request.id));
    const offDeclined = onEvent('request.declined', ({ id }) => without(id));

    return () => {
      cancelled = true;
      offReceived();
      offAccepted();
      offDeclined();
    };
  }, [enabled]);

  return ids.size;
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
  // Height is intrinsic, never fixed: the old `height: 92` was sized for an
  // iPhone home-indicator inset and left ~30px of dead space under the labels on
  // Android. The bottom safe-area inset is carried HERE rather than on Screen so
  // this bar's own background reaches the physical bottom edge (see Screen.tsx).
  root: { position: 'relative', flex: 'none', paddingTop: 10, paddingBottom: 'calc(10px + env(safe-area-inset-bottom, 0px))', display: 'grid', gridTemplateColumns: 'repeat(4,1fr)', alignItems: 'start', borderTop: '1px solid ' + colors.border, background: colors.bgElevated },
  tab: { position: 'relative', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6, textDecoration: 'none', transition: 'color ' + motion.fast },
  tabLabel: { fontSize: 10.5 },
  badge: { position: 'absolute', top: -2, right: 24, minWidth: 17, height: 17, padding: '0 4px', borderRadius: 9, background: colors.accent, color: colors.white, fontSize: 10, fontWeight: 700, lineHeight: '17px', textAlign: 'center' },
  fab: { position: 'absolute', left: '62.5%', top: -26, transform: 'translateX(-50%)', width: 66, height: 66, border: 0, borderRadius: 24, background: colors.accent, boxShadow: shadow.fab, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 2, cursor: 'pointer', transition: 'transform ' + motion.base },
  fabLabel: { fontSize: 9, fontWeight: 700, color: 'rgba(255,255,255,0.9)' },
};
