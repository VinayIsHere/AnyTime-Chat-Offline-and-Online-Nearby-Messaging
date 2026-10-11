import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { UserPlus } from 'lucide-react';

import Screen from '../components/Screen';
import BottomNav from '../components/BottomNav';
import ReportSheet from '../components/ReportSheet';
import { colors, motion } from '../theme/theme';
import { api, type ChatRequest } from '../api/client';
import { onEvent } from '../api/events';

/**
 * Inbound chat requests. BottomNav has always linked /requests; this screen is
 * what it was linking to.
 *
 * Accepting is the moment trust is established: both devices pin the other's
 * authenticated Noise static key onto the contact (docs/ARCHITECTURE.md section
 * 08). That pin is what stops a later device reusing a nickname from
 * impersonating a known contact — so the full handle, discriminator included, is
 * always shown here.
 */
export default function RequestsScreen() {
  const navigate = useNavigate();

  const [requests, setRequests] = useState<ChatRequest[]>([]);
  const [busy, setBusy] = useState<string | null>(null);
  // The request being reported. Held as the whole request, not an id, so the
  // sheet still knows who it is about after the card behind it has gone.
  const [reporting, setReporting] = useState<ChatRequest | null>(null);

  useEffect(() => {
    let cancelled = false;

    api.getRequests().then((list) => {
      if (!cancelled) setRequests(list);
    }).catch(() => {
      if (!cancelled) setRequests([]);
    });

    const off = onEvent('request.received', (req) =>
      setRequests((current) =>
        current.some((r) => r.id === req.id) ? current : [req, ...current],
      ),
    );

    return () => {
      cancelled = true;
      off();
    };
  }, []);

  async function accept(req: ChatRequest) {
    setBusy(req.id);
    try {
      const chat = await api.acceptRequest(req.id);
      setRequests((current) => current.filter((r) => r.id !== req.id));
      navigate('/chats/' + chat.id);
    } catch {
      setBusy(null);
    }
  }

  async function decline(req: ChatRequest, block: boolean) {
    setBusy(req.id);
    try {
      // With block, the core also refuses their announces and future requests,
      // and tells them nothing — being told you were blocked is information.
      await api.declineRequest(req.id, block);
      setRequests((current) => current.filter((r) => r.id !== req.id));
    } catch {
      // The request stays on screen, which is the honest state: it was not
      // declined. The buttons come back so the user can try again.
    } finally {
      setBusy(null);
    }
  }

  return (
    <Screen>
      <div style={requestStyles.header}>
        <h2 style={requestStyles.title}>Requests</h2>
        {requests.length > 0 && <span style={requestStyles.badge}>{requests.length}</span>}
      </div>

      <div style={requestStyles.list}>
        {requests.length === 0 ? (
          <div style={requestStyles.emptyWrap}>
            <span style={requestStyles.emptyMark}>
              <UserPlus size={22} strokeWidth={1.9} color={colors.textFaint} />
            </span>
            <p style={requestStyles.emptyTitle}>No requests yet</p>
            <p style={requestStyles.emptyBody}>
              When someone nearby says hi, it shows up here for you to accept.
            </p>
          </div>
        ) : (
          requests.map((r, i) => (
            <div key={r.id} style={{ ...requestStyles.card, animationDelay: i * 0.06 + 's' }}>
              <div style={requestStyles.cardTop}>
                <span style={requestStyles.avatar}>
                  {(r.nickname.charAt(0) || '?').toUpperCase()}
                </span>
                <span style={requestStyles.cardText}>
                  <span style={requestStyles.cardName}>{r.handle}</span>
                  <span style={requestStyles.cardMeta}>{r.at}</span>
                </span>
              </div>

              {r.greeting && <p style={requestStyles.greeting}>{r.greeting}</p>}

              <div style={requestStyles.actions}>
                <button
                  style={requestStyles.decline}
                  disabled={busy === r.id}
                  onClick={() => decline(r, false)}
                >
                  Decline
                </button>
                <button
                  style={requestStyles.accept}
                  disabled={busy === r.id}
                  onClick={() => accept(r)}
                >
                  {busy === r.id ? 'Accepting…' : 'Accept'}
                </button>
              </div>

              {/* Quieter than the two buttons above on purpose: these are the
                  strong options. A block can be undone from Profile › Blocked. */}
              <div style={requestStyles.quiet}>
                <button
                  style={requestStyles.block}
                  disabled={busy === r.id}
                  onClick={() => decline(r, true)}
                >
                  Decline and block
                </button>
                <span style={requestStyles.dot} aria-hidden="true">·</span>
                {/* A greeting is the first thing a stranger can send, so it is
                    also the first place someone may need to report from. */}
                <button
                  style={requestStyles.block}
                  disabled={busy === r.id}
                  onClick={() => setReporting(r)}
                  aria-label={'Report ' + r.handle}
                >
                  Report
                </button>
              </div>
            </div>
          ))
        )}
      </div>

      <BottomNav requestCount={requests.length} />

      {reporting && (
        <ReportSheet
          peerId={reporting.peerId}
          handle={reporting.handle}
          onClose={() => setReporting(null)}
          // Reporting blocks them, and the core drops a blocked person's
          // waiting request with it — so the card goes too.
          onReported={() =>
            setRequests((current) => current.filter((x) => x.peerId !== reporting.peerId))}
        />
      )}
    </Screen>
  );
}

const requestStyles: Record<string, React.CSSProperties> = {
  header: { display: 'flex', alignItems: 'center', gap: 10, padding: '18px 24px 14px', flex: 'none' },
  title: { margin: 0, fontSize: 22, fontWeight: 800, letterSpacing: '-0.02em' },
  badge: { minWidth: 22, height: 22, padding: '0 7px', borderRadius: 11, background: colors.accent, color: colors.white, fontSize: 12, fontWeight: 700, lineHeight: '22px', textAlign: 'center' },
  list: { flex: 1, overflowY: 'auto', padding: '0 20px 28px', display: 'flex', flexDirection: 'column', gap: 12 },

  emptyWrap: { marginTop: 90, display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 4, padding: '0 34px', textAlign: 'center' },
  emptyMark: { width: 56, height: 56, borderRadius: 18, background: colors.fillSoft, display: 'flex', alignItems: 'center', justifyContent: 'center', marginBottom: 10 },
  emptyTitle: { margin: 0, fontSize: 15, fontWeight: 700 },
  emptyBody: { margin: '6px 0 0', fontSize: 13, fontWeight: 500, lineHeight: 1.55, color: colors.textFaint },

  card: { display: 'flex', flexDirection: 'column', gap: 12, padding: 15, borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border, animation: 'at-softIn 0.4s ease both' },
  cardTop: { display: 'flex', alignItems: 'center', gap: 13 },
  avatar: { width: 44, height: 44, borderRadius: 14, background: colors.avatar, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 15, fontWeight: 700, flex: 'none' },
  cardText: { flex: 1, display: 'flex', flexDirection: 'column', gap: 3, minWidth: 0 },
  cardName: { fontSize: 14.5, fontWeight: 700 },
  cardMeta: { fontSize: 12, fontWeight: 500, color: colors.text45 },
  greeting: { margin: 0, padding: '11px 13px', borderRadius: 13, background: colors.fillSofter, fontSize: 13.5, fontWeight: 500, lineHeight: 1.5, color: colors.text70 },
  actions: { display: 'flex', gap: 9 },
  decline: { flex: 1, height: 42, borderRadius: 13, border: '1px solid ' + colors.borderStrong, background: 'transparent', color: colors.text70, fontSize: 13.5, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer', transition: 'background ' + motion.fast },
  quiet: { alignSelf: 'center', display: 'flex', alignItems: 'center', gap: 4 },
  dot: { color: colors.text35, fontSize: 12.5, fontWeight: 700 },
  block: { padding: '2px 6px', border: 0, background: 'transparent', color: colors.danger, fontSize: 12.5, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer' },
  accept: { flex: 1.4, height: 42, borderRadius: 13, border: 0, background: colors.accent, color: colors.white, fontSize: 13.5, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer', transition: 'background ' + motion.fast },
};
