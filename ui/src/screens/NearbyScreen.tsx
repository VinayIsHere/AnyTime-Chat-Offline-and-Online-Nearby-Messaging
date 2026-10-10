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

  /**
   * Re-reads the whole list from the core. Declared above the effect that uses
   * it, and preferred over merging event payloads.
   *
   * `peer.discovered` carries a SIGHTING, not a status: SseHub cannot see the
   * request gate, so its status fields are neutral placeholders. Merging one in
   * would re-announce an accepted contact as a stranger and clobber a correct
   * row. Discoveries are rare — the core emits one only on first sight — so a
   * refetch per discovery is cheap and always right.
   */
  const refreshPeers = useCallback(() => {
    api.getPeers()
      .then((list) => setPeers([...list].sort(bySignal)))
      .catch(() => {
        /* radio down; peer.lost clears the list */
      });
  }, []);

  useEffect(() => {
    let cancelled = false;

    // Start the radio, then seed from whatever is already reachable. Live
    // updates arrive over SSE from here on.
    api
      .startDiscovery('active')
      .then(() => api.getPeers())
      .then((list) => {
        if (!cancelled) setPeers([...list].sort(bySignal));
      })
      .catch(() => {
        if (!cancelled) setRadioOk(false);
      });

    const offSeen = onEvent('peer.discovered', refreshPeers);
    const offLost = onEvent('peer.lost', ({ peerId }) =>
      setPeers((current) => current.filter((p) => p.peerId !== peerId)),
    );
    const offRadio = onEvent('radio.state', ({ available }) => setRadioOk(available));

    // A decline or an accept changes what this screen should offer, and both
    // arrive while it is open. Refetch rather than guess at the new status.
    const offDeclined = onEvent('request.declined', refreshPeers);
    const offAccepted = onEvent('request.accepted', refreshPeers);

    return () => {
      cancelled = true;
      offSeen();
      offLost();
      offRadio();
      offDeclined();
      offAccepted();

      // Drop to the background duty cycle rather than stopping: discovery has to
      // keep running or inbound requests would never arrive, but a continuous
      // scan is only justified while someone is actually watching this screen.
      api.startDiscovery('passive').catch(() => {
        /* the radio is already down, which is the same outcome */
      });
    };
  }, [refreshPeers]);

  async function sayHi(peer: NearbyPeer) {
    setSending(peer.peerId);
    try {
      await api.sendRequest(peer.peerId);

      // Stay on this screen. Navigating to /requests was wrong twice over: that
      // list holds INBOUND requests, so the one just sent was never there, and
      // being thrown off the radar mid-scan loses whoever else was in range.
      //
      // Refetch rather than patch locally — the server owns the status, and it
      // is the only thing that survives leaving this screen.
      refreshPeers();
    } catch {
      /* the row keeps its previous status; the next refresh corrects it */
    } finally {
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
              <span style={nearbyStyles.cardMeta}>{metaFor(p)}</span>
            </span>
            <button
              style={{
                ...nearbyStyles.hi,
                ...(i === 0 && p.status === 'none' ? nearbyStyles.hiPrimary : null),
                ...(p.status === 'contact' ? nearbyStyles.hiPrimary : null),
                ...(actionFor(p, sending === p.peerId).disabled ? nearbyStyles.hiMuted : null),
              }}
              disabled={actionFor(p, sending === p.peerId).disabled}
              onClick={() => {
                if (p.status === 'contact' && p.chatId) navigate('/chats/' + p.chatId);
                else void sayHi(p);
              }}
            >
              {actionFor(p, sending === p.peerId).label}
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

/**
 * The button, from the server-supplied status.
 *
 * `status` and `canResend` are deliberately separate: an unanswered request
 * stays `requested` — so the row keeps saying it was sent — while becoming
 * tappable again once the one-minute cooldown passes. Collapsing them would
 * either hide that it was sent or leave the button grey forever, which is what
 * happened when a peer simply never answered.
 */
function actionFor(peer: NearbyPeer, inFlight: boolean): { label: string; disabled: boolean } {
  if (peer.status === 'contact') return { label: 'Message', disabled: false };
  if (inFlight) return { label: 'Sending…', disabled: true };

  switch (peer.status) {
    case 'requested':
      return peer.canResend
        ? { label: 'Say hi again', disabled: false }
        : { label: 'Requested', disabled: true };
    case 'declined':
      // A plain decline is told to us, so asking again is allowed. A decline
      // WITH a block is never told to us by design, so that peer stays
      // 'requested' and any resend is refused by the core — silently, which is
      // the intended privacy behaviour.
      return { label: 'Say hi again', disabled: false };
    default:
      return { label: 'Say hi', disabled: false };
  }
}

/** Second line of a peer row: what we want to say about them, else signal. */
function metaFor(peer: NearbyPeer): string {
  switch (peer.status) {
    case 'requested': return peer.canResend ? 'Request sent · no reply yet' : 'Request sent';
    case 'declined':  return 'Request declined';
    case 'contact':   return 'In your chats';
    default:          return peer.signal + ' signal';
  }
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
  list: { flex: 1, overflowY: 'auto', padding: '0 20px 28px', display: 'flex', flexDirection: 'column', gap: 10 },
  empty: { margin: '10px 6px', fontSize: 13, fontWeight: 500, lineHeight: 1.55, color: colors.textFaint, textAlign: 'center' },
  card: { display: 'flex', alignItems: 'center', gap: 13, padding: '13px 14px', borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border, animation: 'at-softIn 0.4s ease both', transition: 'background ' + motion.fast },
  avatar: { width: 44, height: 44, borderRadius: 14, background: colors.avatar, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 15, fontWeight: 700 },
  cardText: { flex: 1, display: 'flex', flexDirection: 'column', gap: 3, minWidth: 0 },
  cardName: { fontSize: 14.5, fontWeight: 700 },
  cardMeta: { fontSize: 12, fontWeight: 500, color: colors.text45 },
  hi: { height: 36, padding: '0 15px', borderRadius: 12, border: '1px solid ' + colors.borderStrong, background: 'transparent', color: colors.text, fontSize: 12.5, fontWeight: 700, fontFamily: 'inherit', whiteSpace: 'nowrap', flex: 'none', cursor: 'pointer', transition: 'background ' + motion.fast },
  hiPrimary: { border: 0, background: colors.accent, color: colors.white },
  // "Requested" during the cooldown: visibly sent, visibly not tappable.
  hiMuted: { background: colors.fillSoft, color: colors.textFaint, border: '1px solid ' + colors.border, cursor: 'default' },
};
