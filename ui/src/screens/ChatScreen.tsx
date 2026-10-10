import React, { useEffect, useRef, useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { ChevronLeft, MoreVertical, Send, Bluetooth, Check, ImagePlus, RotateCw, Ban } from 'lucide-react';
import Screen from '../components/Screen';
import { colors, motion } from '../theme/theme';
import { api, ApiError, type ChatSummary, type DeliveryStatus, type Message } from '../api/client';
import { onEvent } from '../api/events';

export default function ChatScreen() {
  const navigate = useNavigate();
  const { chatId = '' } = useParams();
  const [messages, setMessages] = useState<Message[]>([]);
  // Who this chat is with. Until this was fetched the header had nothing real to
  // render, so it showed a hard-coded name from the design mock.
  const [chat, setChat] = useState<ChatSummary | null>(null);
  const [draft, setDraft] = useState('');
  // Typing indicators need a packet type the v1 protocol does not define — see
  // core/domain/Enums.h (EnvelopeType). The indicator stays wired up but always
  // false, so adding `chat.typing` later is a one-line change here.
  const peerTyping = false;
  const endRef = useRef<HTMLDivElement | null>(null);
  const fileRef = useRef<HTMLInputElement | null>(null);
  const [attaching, setAttaching] = useState(false);
  const [attachError, setAttachError] = useState('');
  const [menuOpen, setMenuOpen] = useState(false);
  const [blockBusy, setBlockBusy] = useState(false);

  // Local blob URLs for photos we are sending, keyed by clientMessageId — the
  // one id that survives the swap from the optimistic row to the stored one, so
  // the picture never reloads and never blinks.
  const [previews, setPreviews] = useState<Record<string, string>>({});
  const previewsRef = useRef(previews);
  previewsRef.current = previews;
  // Every createObjectURL pins its blob in memory until revoked. Leaving the
  // chat is where they stop being useful.
  useEffect(() => () => { Object.values(previewsRef.current).forEach(URL.revokeObjectURL); }, []);

  useEffect(() => {
    let cancelled = false;

    api.getChat(chatId)
      .then((c) => { if (!cancelled) setChat(c); })
      .catch(() => { if (!cancelled) setChat(null); });

    api.getMessages(chatId)
      .then((page) => { if (!cancelled) setMessages([...page.messages].reverse()); })
      .catch(() => { if (!cancelled) setMessages([]); });

    // Inbound messages and delivery-state changes both arrive over SSE (section 09).
    const offRecv = onEvent('message.received', (m) => {
      if (m.chatId === chatId) setMessages((all) => [...all, m]);
    });
    const offStatus = onEvent('message.status', ({ id, status }) =>
      setMessages((all) => all.map((x) => (x.id === id ? { ...x, status } : x))),
    );

    // Presence follows the same announce window the Nearby screen uses, so the
    // header stops claiming "connected" the moment the peer drifts out of range.
    const offSeen = onEvent('peer.discovered', (p) =>
      setChat((c) => (c && c.peerId === p.peerId ? { ...c, reachable: true } : c)),
    );
    const offLost = onEvent('peer.lost', ({ peerId }) =>
      setChat((c) => (c && c.peerId === peerId ? { ...c, reachable: false } : c)),
    );

    return () => { cancelled = true; offRecv(); offStatus(); offSeen(); offLost(); };
  }, [chatId]);
  useEffect(() => { endRef.current?.parentElement?.scrollTo({ top: 1e6, behavior: 'smooth' }); }, [messages, peerTyping]);

  async function send() {
    const body = draft.trim();
    if (!body) return;
    setDraft('');
    // The client id lets the core reconcile this optimistic bubble with the
    // row it stores, so the message never renders twice.
    const clientMessageId = crypto.randomUUID();
    const optimistic: Message = {
      id: 'tmp-' + clientMessageId, chatId, clientMessageId,
      body, mine: true, at: 'now', status: 'queued',
    };
    setMessages((m) => [...m, optimistic]);
    try {
      const saved = await api.sendMessage(chatId, body, clientMessageId);
      setMessages((m) => m.map((x) => (x.id === optimistic.id ? saved : x)));
    } catch {
      setMessages((m) => m.map((x) => (x.id === optimistic.id ? { ...x, status: 'failed' } : x)));
    }
  }

  /**
   * Resends a bubble that says "Not sent". Two different failures end up
   * there, and they need different calls:
   *
   *  - the POST itself failed, so the core never stored the message. There is
   *    no server id ("tmp-..."), so it is POSTed again with the SAME client id —
   *    the core's idempotency rule makes that safe even if the first attempt
   *    did land and only the response was lost.
   *  - the core stored it and later gave up. That row is resent by id.
   */
  async function retry(m: Message) {
    setAttachError('');
    setMessages((all) => all.map((x) => (x.id === m.id ? { ...x, status: 'queued' } : x)));

    try {
      if (m.id.startsWith('tmp-')) {
        const saved = await api.sendMessage(chatId, m.body, m.clientMessageId);
        setMessages((all) => all.map((x) => (x.id === m.id ? saved : x)));
      } else {
        await api.retryMessage(m.id);
        // Later transitions arrive over SSE like any other send.
      }
    } catch (e) {
      setMessages((all) => all.map((x) => (x.id === m.id ? { ...x, status: 'failed' } : x)));
      setAttachError(
        e instanceof ApiError && e.status === 403
          ? 'You blocked this person, so messages cannot be sent.'
          : 'Could not resend that message.',
      );
    }
  }

  async function setBlocked(block: boolean) {
    if (!chat) return;
    setMenuOpen(false);
    setBlockBusy(true);
    setAttachError('');
    try {
      if (block) await api.blockPeer(chat.peerId);
      else await api.unblockPeer(chat.peerId);
      setChat((c) => (c ? { ...c, blocked: block } : c));
    } catch {
      setAttachError(block ? 'Could not block this person.' : 'Could not unblock this person.');
    } finally {
      setBlockBusy(false);
    }
  }

  async function sendImage(file: File) {
    setAttachError('');
    setAttaching(true);

    const clientMessageId = crypto.randomUUID();
    const caption = draft.trim();
    let previewUrl = '';

    try {
      const image = await shrinkForSending(file);

      // Show the photo the moment it is ready, from a local blob URL, the way
      // WhatsApp does. Storing and encrypting it takes 3-5 seconds on a phone,
      // and until now the thread showed nothing at all for that whole stretch.
      //
      // The bubble still appears at "Queued", never "Sent": over BLE the bytes
      // are going out for minutes afterwards, and an instant preview must not
      // be allowed to imply the photo has arrived.
      previewUrl = URL.createObjectURL(image);
      setPreviews((p) => ({ ...p, [clientMessageId]: previewUrl }));
      setDraft('');
      setMessages((all) => [...all, {
        id: 'tmp-' + clientMessageId, chatId, clientMessageId,
        body: caption, mine: true, at: 'now', status: 'queued',
        media: { id: 'local-' + clientMessageId, mimeType: image.type || 'image/jpeg', bytes: image.size },
      }]);

      const saved = await api.sendMedia(chatId, image, caption, clientMessageId);
      // Matched on clientMessageId, not the temporary id: the preview stays
      // keyed to the same photo once the core hands back the real row.
      setMessages((all) => all.map((x) => (x.clientMessageId === clientMessageId ? saved : x)));
    } catch (e) {
      // The core never stored this one, so there is no row to leave behind —
      // drop the bubble rather than stranding a photo that does not exist, and
      // hand the caption back so it is not lost with it.
      if (previewUrl) {
        URL.revokeObjectURL(previewUrl);
        setPreviews((p) => { const next = { ...p }; delete next[clientMessageId]; return next; });
      }
      setMessages((all) => all.filter((x) => x.clientMessageId !== clientMessageId));
      if (caption) setDraft(caption);
      setAttachError(
        e instanceof Error && e.message === 'too-large'
          ? 'That image is too detailed to send over Bluetooth. Try a smaller one.'
          : 'Could not send that image.',
      );
    } finally {
      setAttaching(false);
      if (fileRef.current) fileRef.current.value = ''; // so the same file can be picked again
    }
  }

  return (
    <Screen>
      <div style={chatStyles.header}>
        <button style={chatStyles.iconBtn} onClick={() => navigate(-1)}><ChevronLeft size={18} strokeWidth={2.1} /></button>
        <span style={chatStyles.avatar}>{initialOf(chat?.handle)}</span>
        <span style={chatStyles.peer}>
          <span style={chatStyles.peerName}>{chat?.handle || '…'}</span>
          {/* No distance is shown: BLE gives a coarse signal band, never metres.
              The "~2 m away" this replaced was invented by the design mock. */}
          <span style={{ ...chatStyles.peerMeta, color: chat?.reachable ? colors.success : colors.textFaint }}>
            <Bluetooth size={11} strokeWidth={2.4} />
            {chat?.reachable ? 'connected' : 'not in range'}
          </span>
        </span>
        <button
          style={{ ...chatStyles.iconBtn, color: colors.text60 }}
          onClick={() => setMenuOpen((open) => !open)}
          disabled={!chat}
          aria-label="Chat options"
          aria-expanded={menuOpen}
        >
          <MoreVertical size={18} strokeWidth={2.4} />
        </button>
        {menuOpen && chat && (
          <>
            {/* Catches the tap outside the menu, so it closes the way a native
                menu does instead of needing a second press on the dots. */}
            <div style={chatStyles.scrim} onClick={() => setMenuOpen(false)} />
            <div style={chatStyles.menu} role="menu">
              <button
                role="menuitem"
                style={{ ...chatStyles.menuItem, color: chat.blocked ? colors.text : colors.danger }}
                onClick={() => setBlocked(!chat.blocked)}
              >
                <Ban size={16} strokeWidth={2.2} />
                {chat.blocked ? 'Unblock ' : 'Block '}{chat.handle}
              </button>
            </div>
          </>
        )}
      </div>

      <div style={chatStyles.thread}>
        <span style={chatStyles.pill}>
          {chat?.reachable
            ? 'CONNECTED OVER BLUETOOTH · NO INTERNET'
            : 'OUT OF RANGE · MESSAGES WILL QUEUE'}
        </span>
        {messages.map((m) => (
          <div key={m.id} style={m.mine ? chatStyles.mineWrap : chatStyles.theirsWrap}>
            {m.media ? (
              <div style={m.mine ? chatStyles.mine : chatStyles.theirs}>
                {/* A local blob URL while the photo is still on its way out,
                    otherwise the same-origin URL carrying the secret prefix —
                    where the core sniffs the type from the bytes rather than
                    trusting anything sent here. Dimmed until it is stored, so
                    "still going" is visible without reading the receipt. */}
                <img
                  src={previews[m.clientMessageId] ?? api.mediaUrl(m.media.id)}
                  alt={m.body || 'Photo'}
                  style={{ ...chatStyles.image, opacity: m.status === 'queued' ? 0.65 : 1 }}
                />
                {m.body && <div style={chatStyles.caption}>{m.body}</div>}
              </div>
            ) : (
              <div style={m.mine ? chatStyles.mine : chatStyles.theirs}>{m.body}</div>
            )}
            {m.mine && m.status === 'failed' ? (
              // The only receipt that asks for something, so it is a button:
              // "Not sent" with no way forward was a dead end.
              <button style={chatStyles.retry} onClick={() => retry(m)} aria-label="Retry sending">
                {m.at} · {receiptLabel(m.status)} · Tap to retry
                <RotateCw size={12} strokeWidth={2.4} />
              </button>
            ) : m.mine && (
              <span style={chatStyles.receipt}>
                {m.at} · {receiptLabel(m.status)}
                {m.status === 'delivered' && <Check size={13} strokeWidth={2.4} color={colors.success} />}
              </span>
            )}
          </div>
        ))}
        {peerTyping && (
          <div style={chatStyles.typing}>
            {[0, 0.2, 0.4].map((d) => <span key={d} style={{ ...chatStyles.typingDot, animationDelay: d + 's' }} />)}
          </div>
        )}
        <div ref={endRef} />
      </div>

      {attachError && <div style={chatStyles.attachError}>{attachError}</div>}

      {chat?.blocked ? (
        // Not a disabled composer: a send button that can only fail invites
        // the tap. Say what the block means and offer the way back.
        <div style={chatStyles.blockedBar}>
          <span style={chatStyles.blockedText}>
            You blocked {chat.handle}. Neither of you can message the other.
          </span>
          <button style={chatStyles.unblock} onClick={() => setBlocked(false)} disabled={blockBusy}>
            {blockBusy ? 'Unblocking…' : 'Unblock'}
          </button>
        </div>
      ) : (
        <div style={chatStyles.composer}>
          <input
            ref={fileRef}
            type="file"
            accept="image/*"
            style={{ display: 'none' }}
            onChange={(e) => {
              const file = e.target.files?.[0];
              if (file) void sendImage(file);
            }}
          />
          <button
            style={chatStyles.attach}
            onClick={() => fileRef.current?.click()}
            disabled={attaching}
            aria-label="Send a photo"
          >
            <ImagePlus size={20} strokeWidth={2} />
          </button>

          <div style={chatStyles.inputWrap}>
            <input
              value={draft}
              onChange={(e) => setDraft(e.target.value)}
              onKeyDown={(e) => { if (e.key === 'Enter') send(); }}
              placeholder={attaching ? 'Sending photo…' : 'Message'}
              style={chatStyles.input}
            />
          </div>
          <button style={chatStyles.send} onClick={send} aria-label="Send"><Send size={20} strokeWidth={2} /></button>
        </div>
      )}
    </Screen>
  );
}

/**
 * The receipt under one's own message.
 *
 * 'failed' used to fall through to "Sent", which told the user the exact
 * opposite of what had happened to their message.
 */
function receiptLabel(status: DeliveryStatus): string {
  switch (status) {
    case 'queued':    return 'Queued';
    case 'delivered': return 'Delivered';
    case 'read':      return 'Read';
    case 'failed':    return 'Not sent';
    default:          return 'Sent';
  }
}

/** First letter of a handle, ignoring the leading "@". */
function initialOf(handle: string | undefined): string {
  return (handle?.replace('@', '').charAt(0) || '?').toUpperCase();
}

/**
 * Mirrors protocol::kMaxMediaBytes. The core rejects anything larger, so this
 * is the number the UI has to land under — not a suggestion.
 */
const MAX_IMAGE_BYTES = 192 * 1024;

/** Longest edge after downscaling. Plenty for a phone screen. */
const MAX_IMAGE_EDGE = 1600;

/**
 * Downscales and re-encodes a photo until it fits.
 *
 * This has to happen HERE, in the only layer that can decode an image without
 * dragging an image library into the C++ core. A modern phone camera produces
 * 3-8 MB originals; sending one of those over BLE at roughly 1-2 KB/s would
 * take the better part of an hour, so the compression is not a nicety.
 *
 * Quality steps down rather than binary-searching: re-encoding costs
 * milliseconds, while every extra kilobyte that survives costs about another
 * second on the radio.
 */
async function shrinkForSending(file: File): Promise<Blob> {
  const bitmap = await createImageBitmap(file);

  const scale = Math.min(1, MAX_IMAGE_EDGE / Math.max(bitmap.width, bitmap.height));
  const canvas = document.createElement('canvas');
  canvas.width = Math.max(1, Math.round(bitmap.width * scale));
  canvas.height = Math.max(1, Math.round(bitmap.height * scale));

  const ctx = canvas.getContext('2d');
  if (!ctx) throw new Error('no-canvas');
  ctx.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
  bitmap.close?.();

  for (const quality of [0.82, 0.7, 0.6, 0.5, 0.4]) {
    const blob = await new Promise<Blob | null>((resolve) =>
      canvas.toBlob(resolve, 'image/jpeg', quality),
    );
    if (blob && blob.size <= MAX_IMAGE_BYTES) return blob;
  }
  // Every quality step still too big — almost always a very large, very noisy
  // photo. Refusing beats sending something the core will reject anyway.
  throw new Error('too-large');
}

const chatStyles: Record<string, React.CSSProperties> = {
  header: { position: 'relative', display: 'flex', alignItems: 'center', gap: 12, padding: '14px 18px', borderBottom: '1px solid ' + colors.border, flex: 'none' },
  iconBtn: { width: 34, height: 34, borderRadius: 11, border: 0, background: 'transparent', color: colors.text, display: 'flex', alignItems: 'center', justifyContent: 'center', cursor: 'pointer', transition: 'background ' + motion.fast },
  avatar: { width: 40, height: 40, borderRadius: 13, background: colors.avatar, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 14, fontWeight: 700 },
  peer: { flex: 1, display: 'flex', flexDirection: 'column', gap: 2, minWidth: 0 },
  peerName: { fontSize: 15, fontWeight: 700 },
  peerMeta: { display: 'flex', alignItems: 'center', gap: 5, fontSize: 11.5, fontWeight: 600, color: colors.success },
  thread: { flex: 1, overflowY: 'auto', padding: '18px 18px 8px', display: 'flex', flexDirection: 'column', gap: 10 },
  pill: { alignSelf: 'center', padding: '5px 12px', borderRadius: 999, background: colors.fillSoft, fontSize: 10.5, fontWeight: 600, color: colors.textFaint, letterSpacing: '0.03em' },
  theirsWrap: { maxWidth: '76%', alignSelf: 'flex-start', animation: 'at-softIn 0.3s ease both' },
  mineWrap: { maxWidth: '76%', width: 'fit-content', alignSelf: 'flex-end', display: 'flex', flexDirection: 'column', gap: 5, alignItems: 'flex-end', animation: 'at-softIn 0.3s ease both' },
  theirs: { padding: '11px 14px', borderRadius: '18px 18px 18px 6px', background: colors.bubble, fontSize: 14.5, fontWeight: 500, lineHeight: 1.45 },
  mine: { padding: '11px 14px', borderRadius: '18px 18px 6px 18px', background: colors.accent, color: colors.white, fontSize: 14.5, fontWeight: 500, lineHeight: 1.45 },
  receipt: { display: 'flex', alignItems: 'center', gap: 5, fontSize: 10.5, fontWeight: 600, color: colors.textFaint },
  typing: { alignSelf: 'flex-start', display: 'flex', gap: 4, padding: '13px 15px', borderRadius: '18px 18px 18px 6px', background: colors.bubble },
  typingDot: { width: 6, height: 6, borderRadius: '50%', background: colors.text50, display: 'block', animation: 'at-blink 1.2s steps(1) infinite' },
  // Owns the bottom safe-area inset itself — Screen no longer pads for it.
  composer: { flex: 'none', padding: '10px 16px 0', paddingBottom: 'calc(18px + env(safe-area-inset-bottom, 0px))', display: 'flex', alignItems: 'center', gap: 10, borderTop: '1px solid ' + colors.border },
  inputWrap: { flex: 1, display: 'flex', alignItems: 'center', gap: 10, height: 50, padding: '0 16px', borderRadius: 25, background: colors.fillSoft, border: '1px solid rgba(255,255,255,0.08)' },
  input: { flex: 1, minWidth: 0, background: 'transparent', border: 0, outline: 'none', color: colors.text, fontSize: 14.5, fontWeight: 500, fontFamily: 'inherit' },
  send: { width: 50, height: 50, borderRadius: 25, border: 0, background: colors.accent, color: colors.white, display: 'flex', alignItems: 'center', justifyContent: 'center', cursor: 'pointer', boxShadow: '0 8px 22px rgba(68,83,246,0.3)', transition: 'background ' + motion.fast },
  attach: { width: 44, height: 44, borderRadius: 22, border: 0, background: colors.fillSoft, color: colors.text60, display: 'flex', alignItems: 'center', justifyContent: 'center', flex: 'none', cursor: 'pointer', transition: 'background ' + motion.fast },
  // maxWidth 100% so a wide photo cannot push the bubble past the 76% column.
  image: { display: 'block', maxWidth: '100%', borderRadius: 12, margin: '-2px 0' },
  caption: { marginTop: 8, fontSize: 14.5, fontWeight: 500, lineHeight: 1.45 },
  // The header is the positioning context for the menu below the dots.
  scrim: { position: 'fixed', inset: 0, zIndex: 10 },
  menu: { position: 'absolute', top: 58, right: 14, zIndex: 11, minWidth: 200, padding: 6, borderRadius: 14, background: colors.surface, border: '1px solid ' + colors.border, boxShadow: '0 12px 32px rgba(18,18,26,0.18)', animation: 'at-softIn 0.18s ease both' },
  menuItem: { width: '100%', display: 'flex', alignItems: 'center', gap: 10, padding: '11px 12px', border: 0, borderRadius: 10, background: 'transparent', fontSize: 13.5, fontWeight: 700, fontFamily: 'inherit', textAlign: 'left', cursor: 'pointer' },
  retry: { display: 'flex', alignItems: 'center', gap: 5, padding: 0, border: 0, background: 'transparent', fontSize: 10.5, fontWeight: 700, fontFamily: 'inherit', color: colors.danger, cursor: 'pointer' },
  blockedBar: { flex: 'none', padding: '14px 18px 0', paddingBottom: 'calc(18px + env(safe-area-inset-bottom, 0px))', display: 'flex', alignItems: 'center', gap: 12, borderTop: '1px solid ' + colors.border },
  blockedText: { flex: 1, fontSize: 12.5, fontWeight: 600, lineHeight: 1.45, color: colors.text60 },
  unblock: { flex: 'none', height: 40, padding: '0 18px', borderRadius: 12, border: '1px solid ' + colors.borderStrong, background: colors.surface, color: colors.text, fontSize: 13, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer' },
  attachError: { flex: 'none', margin: '0 18px 8px', padding: '10px 13px', borderRadius: 12, background: colors.fillSoft, fontSize: 12.5, fontWeight: 600, color: colors.danger },
};
