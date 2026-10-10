import React, { useEffect, useRef, useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { ChevronLeft, MoreVertical, Send, Bluetooth, Check } from 'lucide-react';
import Screen from '../components/Screen';
import { colors, motion, shadow } from '../theme/theme';
import { api, Message } from '../api/client';

export default function ChatScreen() {
  const navigate = useNavigate();
  const { chatId = '' } = useParams();
  const [messages, setMessages] = useState<Message[]>([]);
  const [draft, setDraft] = useState('');
  const [peerTyping, setPeerTyping] = useState(false);
  const endRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => { api.getMessages(chatId).then(setMessages).catch(() => setMessages([])); }, [chatId]);
  useEffect(() => { endRef.current?.parentElement?.scrollTo({ top: 1e6, behavior: 'smooth' }); }, [messages, peerTyping]);

  async function send() {
    const body = draft.trim();
    if (!body) return;
    setDraft('');
    const optimistic: Message = { id: 'tmp-' + Date.now(), chatId, body, mine: true, at: 'now', status: 'queued' };
    setMessages((m) => [...m, optimistic]);
    try {
      const saved = await api.sendMessage(chatId, body);
      setMessages((m) => m.map((x) => (x.id === optimistic.id ? saved : x)));
    } catch {
      setMessages((m) => m.map((x) => (x.id === optimistic.id ? { ...x, status: 'queued' } : x)));
    }
  }

  return (
    <Screen>
      <div style={chatStyles.header}>
        <button style={chatStyles.iconBtn} onClick={() => navigate(-1)}><ChevronLeft size={18} strokeWidth={2.1} /></button>
        <span style={chatStyles.avatar}>A</span>
        <span style={chatStyles.peer}>
          <span style={chatStyles.peerName}>@arjun_k</span>
          <span style={chatStyles.peerMeta}><Bluetooth size={11} strokeWidth={2.4} />~2 m away · connected</span>
        </span>
        <button style={{ ...chatStyles.iconBtn, color: colors.text60 }}><MoreVertical size={18} strokeWidth={2.4} /></button>
      </div>

      <div style={chatStyles.thread}>
        <span style={chatStyles.pill}>CONNECTED OVER BLUETOOTH · NO INTERNET</span>
        {messages.map((m) => (
          <div key={m.id} style={m.mine ? chatStyles.mineWrap : chatStyles.theirsWrap}>
            <div style={m.mine ? chatStyles.mine : chatStyles.theirs}>{m.body}</div>
            {m.mine && (
              <span style={chatStyles.receipt}>
                {m.at} · {m.status === 'queued' ? 'Queued' : m.status === 'delivered' ? 'Delivered' : 'Sent'}
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

      <div style={chatStyles.composer}>
        <div style={chatStyles.inputWrap}>
          <input
            value={draft}
            onChange={(e) => setDraft(e.target.value)}
            onKeyDown={(e) => { if (e.key === 'Enter') send(); }}
            placeholder="Message"
            style={chatStyles.input}
          />
        </div>
        <button style={chatStyles.send} onClick={send} aria-label="Send"><Send size={20} strokeWidth={2} /></button>
      </div>
    </Screen>
  );
}

const chatStyles: Record<string, React.CSSProperties> = {
  header: { display: 'flex', alignItems: 'center', gap: 12, padding: '14px 18px', borderBottom: '1px solid ' + colors.border, flex: 'none' },
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
  composer: { flex: 'none', padding: '10px 16px 30px', display: 'flex', alignItems: 'center', gap: 10, borderTop: '1px solid ' + colors.border },
  inputWrap: { flex: 1, display: 'flex', alignItems: 'center', gap: 10, height: 50, padding: '0 16px', borderRadius: 25, background: colors.fillSoft, border: '1px solid rgba(255,255,255,0.08)' },
  input: { flex: 1, minWidth: 0, background: 'transparent', border: 0, outline: 'none', color: colors.text, fontSize: 14.5, fontWeight: 500, fontFamily: 'inherit' },
  send: { width: 50, height: 50, borderRadius: 25, border: 0, background: colors.accent, color: colors.white, display: 'flex', alignItems: 'center', justifyContent: 'center', cursor: 'pointer', boxShadow: '0 8px 22px rgba(68,83,246,0.3)', transition: 'background ' + motion.fast },
};
