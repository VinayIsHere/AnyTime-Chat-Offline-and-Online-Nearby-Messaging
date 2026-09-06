import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Bluetooth, BluetoothOff } from 'lucide-react';

import Screen from '../components/Screen';
import BottomNav from '../components/BottomNav';
import { colors, motion } from '../theme/theme';
import { api, type NearbyPeer } from '../api/client';
import { onEvent } from '../api/events';

const RING_POSITIONS: React.CSSProperties[] = [
  { top: 44, left: 98 },
  { top: 130, right: 66 },
  { bottom: 56, left: 118 },
];

/**
 * The radar. Peers appear as their signed ANNOUNCE packets arrive and disappear
 * when they fall outside the 60 s reachability window — liveness never comes
 * from link events (docs/ARCHITECTURE.md section 11).
 *
 * This screen SUBSCRIBES rather than polls: the original draft re-fetched every
 * 2.5 s, which burned battery and still lagged behind the radio. One SSE stream
 * carries peer.discovered / peer.lost as they happen.
 */
export default function NearbyScreen() {
  const navigate = useNavigate();

  const [peers, setPeers] = useState<NearbyPeer[]>([]);
  const [radioOk, setRadioOk] = useState(true);
  const [sending, setSending] = useState<string | null>(null);

  const upsert = useCallback((peer: NearbyPeer) => {
    setPeers((current) => {
      const next = current.filter((p) => p.peerId !== peer.peerId);
      next.push(peer);
      return next.sort(bySignal);
    });
  }, []);

  useEffect(() => {
    let cancelled = false;

    // Start the radio, then seed from whatever is already reachable. Live
    // updates arrive over SSE from here on.
    api
      .startDiscovery()
      .then(() => api.getPeers())
      .then((list) => {
        if (!cancelled) setPeers([...list].sort(bySignal));
      })
      .catch(() => {
        if (!cancelled) setRadioOk(false);
      });

    const offSeen = onEvent('peer.discovered', upsert);
    const offLost = onEvent('peer.lost', ({ peerId }) =>
      setPeers((current) => current.filter((p) => p.peerId !== peerId)),
    );
    const offRadio = onEvent('radio.state', ({ available }) => setRadioOk(available));

    return () => {
      cancelled = true;
      offSeen();
      offLost();
      offRadio();
      // Discovery keeps running in the background so requests can still arrive;
      // phase 4 drops it to a passive duty cycle when this screen is not visible.
    };
  }, [upsert]);

  async function sayHi(peer: NearbyPeer) {
    setSending(peer.peerId);
    try {
      await api.sendRequest(peer.peerId);
      navigate('/requests');
    } catch {
      setSending(null);
    }
  }

  return (
    <Screen>
      <div style={nearbyStyles.header}>
        <h2 style={nearbyStyles.title}>Nearby</h2>
        <span style={{ ...nearbyStyles.btChip, ...(radioOk ? null : nearbyStyles.btChipOff) }}>
          {radioOk ? <Bluetooth size={13} strokeWidth={2.2} /> : <BluetoothOff size={13} strokeWidth={2.2} />}
          {radioOk ? 'Bluetooth on' : 'Bluetooth off'}
        </span>
      </div>

      <div style={nearbyStyles.radar}>
        <div style={nearbyStyles.sweep} />
        {[0, 1, 2].map((i) => (
          <span key={i} style={{ ...nearbyStyles.ring, animationDelay: i + 's' }} />
        ))}
        <span style={nearbyStyles.ringStatic} />
        <span style={nearbyStyles.ringInner} />
        <span style={nearbyStyles.me}>•</span>
        {peers.slice(0, 3).map((p, i) => (
          <span
            key={p.peerId}
            style={{ ...nearbyStyles.blip, ...RING_POSITIONS[i], animationDelay: i * 0.15 + 's' }}
          >
            {initialOf(p.nickname)}
          </span>
        ))}
      </div>

      <div style={nearbyStyles.countRow}>
        <span style={nearbyStyles.count}>
          {peers.length} {peers.length === 1 ? 'person' : 'people'} nearby
        </span>
        <span style={nearbyStyles.status}>{radioOk ? 'Listening…' : 'Radio unavailable'}</span>
      </div>

      <div style={nearbyStyles.list}>
        {peers.length === 0 && radioOk && (
          <p style={nearbyStyles.empty}>
            Nobody in range yet. AnyTime keeps listening while the app is open.
          </p>
        )}

        {peers.map((p, i) => (
          <div key={p.peerId} style={{ ...nearbyStyles.card, animationDelay: i * 0.06 + 's' }}>
            <span style={nearbyStyles.avatar}>{initialOf(p.nickname)}</span>
            <span style={nearbyStyles.cardText}>
              {/* The full handle, including the fingerprint suffix — this is a
                  trust decision, so the discriminator is always shown here. */}
              <span style={nearbyStyles.cardName}>{p.handle}</span>
              <span style={nearbyStyles.cardMeta}>{p.signal} signal</span>
            </span>
            <button
              style={{ ...nearbyStyles.hi, ...(i === 0 ? nearbyStyles.hiPrimary : null) }}
              disabled={sending === p.peerId}
              onClick={() => sayHi(p)}
            >
              {sending === p.peerId ? 'Sending…' : 'Say hi'}
            </button>
          </div>
        ))}
      </div>

      <BottomNav />
    </Screen>
  );
}

const SIGNAL_ORDER: Record<NearbyPeer['signal'], number> = {
  'very strong': 0,
  strong: 1,
  weak: 2,
  unknown: 3,
};

function bySignal(a: NearbyPeer, b: NearbyPeer): number {
  return SIGNAL_ORDER[a.signal] - SIGNAL_ORDER[b.signal];
}

function initialOf(nickname: string): string {
  return (nickname.replace('@', '').charAt(0) || '?').toUpperCase();
}

const nearbyStyles: Record<string, React.CSSProperties> = {
  header: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '18px 24px 6px', flex: 'none' },
  title: { margin: 0, fontSize: 22, fontWeight: 800, letterSpacing: '-0.02em' },
  btChip: { display: 'flex', alignItems: 'center', gap: 7, padding: '7px 12px', borderRadius: 999, background: colors.accentSoft, border: '1px solid ' + colors.accentBorder, fontSize: 11.5, fontWeight: 700, color: colors.accentText },
  btChipOff: { background: colors.fillSoft, border: '1px solid ' + colors.border, color: colors.textFaint },
  radar: { position: 'relative', flex: 'none', height: 296, display: 'flex', alignItems: 'center', justifyContent: 'center' },
  sweep: { position: 'absolute', width: 250, height: 250, borderRadius: '50%', background: 'conic-gradient(from 0deg, rgba(68,83,246,0) 0deg, rgba(68,83,246,0.12) 300deg, rgba(68,83,246,0.3) 356deg, rgba(68,83,246,0) 360deg)', animation: 'at-sweep 3.4s linear infinite' },
  ring: { position: 'absolute', width: 250, height: 250, borderRadius: '50%', border: '1px solid rgba(68,83,246,0.45)', animation: 'at-ringPulse 3s ease-out infinite' },
  ringStatic: { position: 'absolute', width: 250, height: 250, borderRadius: '50%', border: '1px dashed ' + colors.border },
  ringInner: { position: 'absolute', width: 160, height: 160, borderRadius: '50%', border: '1px dashed ' + colors.border },
  me: { position: 'relative', width: 60, height: 60, borderRadius: '50%', background: colors.accent, color: colors.white, boxShadow: '0 0 0 8px rgba(68,83,246,0.16), 0 10px 26px rgba(68,83,246,0.5)', display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 17, fontWeight: 800 },
  blip: { position: 'absolute', width: 46, height: 46, borderRadius: '50%', background: colors.surface, border: '1px solid ' + colors.borderStrong, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 14, fontWeight: 700, animation: 'at-softIn 0.5s ease both' },
  countRow: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '2px 24px 12px', flex: 'none' },
  count: { fontSize: 13, fontWeight: 700 },
  status: { fontSize: 12, fontWeight: 600, color: colors.textFaint },
  list: { flex: 1, overflowY: 'auto', padding: '0 20px', display: 'flex', flexDirection: 'column', gap: 10 },
  empty: { margin: '10px 6px', fontSize: 13, fontWeight: 500, lineHeight: 1.55, color: colors.textFaint, textAlign: 'center' },
  card: { display: 'flex', alignItems: 'center', gap: 13, padding: '13px 14px', borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border, animation: 'at-softIn 0.4s ease both', transition: 'background ' + motion.fast },
  avatar: { width: 44, height: 44, borderRadius: 14, background: colors.avatar, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 15, fontWeight: 700 },
  cardText: { flex: 1, display: 'flex', flexDirection: 'column', gap: 3, minWidth: 0 },
  cardName: { fontSize: 14.5, fontWeight: 700 },
  cardMeta: { fontSize: 12, fontWeight: 500, color: colors.text45 },
  hi: { height: 36, padding: '0 15px', borderRadius: 12, border: '1px solid ' + colors.borderStrong, background: 'transparent', color: colors.text, fontSize: 12.5, fontWeight: 700, fontFamily: 'inherit', whiteSpace: 'nowrap', flex: 'none', cursor: 'pointer', transition: 'background ' + motion.fast },
  hiPrimary: { border: 0, background: colors.accent, color: colors.white },
};
