import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Bluetooth } from 'lucide-react';
import Screen from '../components/Screen';
import BottomNav from '../components/BottomNav';
import { colors, motion, shadow } from '../theme/theme';
import { api, NearbyUser } from '../api/client';

const RING_POSITIONS: React.CSSProperties[] = [
  { top: 44, left: 98 },
  { top: 130, right: 66 },
  { bottom: 56, left: 118 },
];

export default function NearbyScreen() {
  const navigate = useNavigate();
  const [scanning, setScanning] = useState(true);
  const [users, setUsers] = useState<NearbyUser[]>([]);

  useEffect(() => {
    let stop = false;
    let timer: number | undefined;
    (async () => {
      const { scanId } = await api.startScan();
      const poll = async () => {
        if (stop) return;
        try { setUsers(await api.getNearby(scanId)); } catch {}
        timer = window.setTimeout(poll, 2500);
      };
      poll();
      window.setTimeout(() => setScanning(false), 6000);
    })();
    return () => { stop = true; if (timer) clearTimeout(timer); };
  }, []);

  async function sayHi(u: NearbyUser) {
    const { chatId } = await api.sayHi(u.id);
    navigate('/chats/' + chatId);
  }

  return (
    <Screen>
      <div style={nearbyStyles.header}>
        <h2 style={nearbyStyles.title}>Nearby</h2>
        <span style={nearbyStyles.btChip}><Bluetooth size={13} strokeWidth={2.2} />Bluetooth on</span>
      </div>

      <div style={nearbyStyles.radar}>
        <div style={nearbyStyles.sweep} />
        {[0, 1, 2].map((i) => (
          <span key={i} style={{ ...nearbyStyles.ring, animationDelay: i + 's' }} />
        ))}
        <span style={nearbyStyles.ringStatic} />
        <span style={nearbyStyles.ringInner} />
        <span style={nearbyStyles.me}>V</span>
        {users.slice(0, 3).map((u, i) => (
          <span key={u.id} style={{ ...nearbyStyles.blip, ...RING_POSITIONS[i], animationDelay: i * 0.15 + 's' }}>
            {u.username.replace('@', '').charAt(0).toUpperCase()}
          </span>
        ))}
      </div>

      <div style={nearbyStyles.countRow}>
        <span style={nearbyStyles.count}>{users.length} {users.length === 1 ? 'person' : 'people'} nearby</span>
        <span style={nearbyStyles.status}>{scanning ? 'Scanning…' : 'Up to date'}</span>
      </div>

      <div style={nearbyStyles.list}>
        {users.map((u, i) => (
          <div key={u.id} style={{ ...nearbyStyles.card, animationDelay: i * 0.06 + 's' }}>
            <span style={nearbyStyles.avatar}>{u.username.replace('@', '').charAt(0).toUpperCase()}</span>
            <span style={nearbyStyles.cardText}>
              <span style={nearbyStyles.cardName}>@{u.username.replace('@', '')}</span>
              <span style={nearbyStyles.cardMeta}>~{u.distanceMeters} m · {u.signal} signal</span>
            </span>
            <button style={{ ...nearbyStyles.hi, ...(i === 0 ? nearbyStyles.hiPrimary : null) }} onClick={() => sayHi(u)}>Say hi</button>
          </div>
        ))}
      </div>

      <BottomNav />
    </Screen>
  );
}

const nearbyStyles: Record<string, React.CSSProperties> = {
  header: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '18px 24px 6px', flex: 'none' },
  title: { margin: 0, fontSize: 22, fontWeight: 800, letterSpacing: '-0.02em' },
  btChip: { display: 'flex', alignItems: 'center', gap: 7, padding: '7px 12px', borderRadius: 999, background: colors.accentSoft, border: '1px solid ' + colors.accentBorder, fontSize: 11.5, fontWeight: 700, color: '#A9B2FF' },
  radar: { position: 'relative', flex: 'none', height: 296, display: 'flex', alignItems: 'center', justifyContent: 'center' },
  sweep: { position: 'absolute', width: 250, height: 250, borderRadius: '50%', background: 'conic-gradient(from 0deg, rgba(68,83,246,0) 0deg, rgba(68,83,246,0.12) 300deg, rgba(68,83,246,0.3) 356deg, rgba(68,83,246,0) 360deg)', animation: 'at-sweep 3.4s linear infinite' },
  ring: { position: 'absolute', width: 250, height: 250, borderRadius: '50%', border: '1px solid rgba(68,83,246,0.45)', animation: 'at-ringPulse 3s ease-out infinite' },
  ringStatic: { position: 'absolute', width: 250, height: 250, borderRadius: '50%', border: '1px dashed rgba(255,255,255,0.07)' },
  ringInner: { position: 'absolute', width: 160, height: 160, borderRadius: '50%', border: '1px dashed rgba(255,255,255,0.06)' },
  me: { position: 'relative', width: 60, height: 60, borderRadius: '50%', background: colors.accent, boxShadow: '0 0 0 8px rgba(68,83,246,0.16), 0 10px 26px rgba(68,83,246,0.5)', display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 17, fontWeight: 800 },
  blip: { position: 'absolute', width: 46, height: 46, borderRadius: '50%', background: colors.surface, border: '1px solid rgba(255,255,255,0.12)', display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 14, fontWeight: 700, animation: 'at-softIn 0.5s ease both' },
  countRow: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '2px 24px 12px', flex: 'none' },
  count: { fontSize: 13, fontWeight: 700 },
  status: { fontSize: 12, fontWeight: 600, color: colors.textFaint },
  list: { flex: 1, overflowY: 'auto', padding: '0 20px', display: 'flex', flexDirection: 'column', gap: 10 },
  card: { display: 'flex', alignItems: 'center', gap: 13, padding: '13px 14px', borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border, animation: 'at-softIn 0.4s ease both', transition: 'background ' + motion.fast },
  avatar: { width: 44, height: 44, borderRadius: 14, background: colors.avatar, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 15, fontWeight: 700 },
  cardText: { flex: 1, display: 'flex', flexDirection: 'column', gap: 3, minWidth: 0 },
  cardName: { fontSize: 14.5, fontWeight: 700 },
  cardMeta: { fontSize: 12, fontWeight: 500, color: colors.text45 },
  hi: { height: 36, padding: '0 15px', borderRadius: 12, border: '1px solid ' + colors.borderStrong, background: 'transparent', color: colors.text, fontSize: 12.5, fontWeight: 700, fontFamily: 'inherit', whiteSpace: 'nowrap', flex: 'none', cursor: 'pointer', transition: 'background ' + motion.fast },
  hiPrimary: { border: 0, background: colors.accent, color: colors.white },
};
