import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Search, Radar } from 'lucide-react';
import Screen from '../components/Screen';
import BottomNav from '../components/BottomNav';
import { colors, motion } from '../theme/theme';
import { api, ChatSummary } from '../api/client';

export default function ChatsScreen() {
  const navigate = useNavigate();
  const [chats, setChats] = useState<ChatSummary[]>([]);
  const [query, setQuery] = useState('');

  useEffect(() => { api.getChats().then(setChats).catch(() => setChats([])); }, []);

  const shown = chats.filter((c) => c.username.includes(query.toLowerCase()));

  return (
    <Screen>
      <div style={chatsStyles.header}>
        <h2 style={chatsStyles.title}>Chats</h2>
        <span style={chatsStyles.mesh}><span style={chatsStyles.dot} />Bluetooth mesh active</span>
      </div>

      <div style={chatsStyles.searchWrap}>
        <div style={chatsStyles.search}>
          <Search size={16} strokeWidth={2} color={colors.textFaint} />
          <input value={query} onChange={(e) => setQuery(e.target.value)} placeholder="Search chats" style={chatsStyles.searchInput} />
        </div>
      </div>

      <div style={chatsStyles.list}>
        {shown.map((c) => (
          <div key={c.id} style={{ ...chatsStyles.row, opacity: c.online ? 1 : 0.62 }} onClick={() => navigate('/chats/' + c.id)}>
            <span style={chatsStyles.avatar}>
              {c.username.charAt(0).toUpperCase()}
              <span style={{ ...chatsStyles.presence, background: c.online ? colors.success : colors.offline }} />
            </span>
            <span style={chatsStyles.rowText}>
              <span style={chatsStyles.rowTop}>
                <span style={chatsStyles.name}>@{c.username}</span>
                <span style={{ ...chatsStyles.time, color: c.unread ? colors.accentText : colors.text35 }}>{c.at}</span>
              </span>
              <span style={chatsStyles.rowBottom}>
                <span style={chatsStyles.preview}>{c.lastMessage}</span>
                {c.unread > 0 && <span style={chatsStyles.unread}>{c.unread}</span>}
              </span>
            </span>
          </div>
        ))}

        <div style={chatsStyles.empty}>
          <Radar size={22} strokeWidth={1.9} color={colors.accentText} />
          <p style={chatsStyles.emptyText}>Tap Nearby to find more people around you</p>
        </div>
      </div>

      <BottomNav />
    </Screen>
  );
}

const chatsStyles: Record<string, React.CSSProperties> = {
  header: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '18px 24px 14px', flex: 'none' },
  title: { margin: 0, fontSize: 22, fontWeight: 800, letterSpacing: '-0.02em' },
  mesh: { display: 'flex', alignItems: 'center', gap: 7, fontSize: 11.5, fontWeight: 600, color: colors.text45 },
  dot: { width: 7, height: 7, borderRadius: '50%', background: colors.success, display: 'block' },
  searchWrap: { padding: '0 20px 12px', flex: 'none' },
  search: { display: 'flex', alignItems: 'center', gap: 10, height: 46, padding: '0 14px', borderRadius: 14, background: colors.surface, border: '1px solid ' + colors.border },
  searchInput: { flex: 1, minWidth: 0, background: 'transparent', border: 0, outline: 'none', color: colors.text, fontSize: 14, fontWeight: 500, fontFamily: 'inherit' },
  list: { flex: 1, overflowY: 'auto', display: 'flex', flexDirection: 'column' },
  row: { display: 'flex', alignItems: 'center', gap: 13, padding: '14px 22px', cursor: 'pointer', transition: 'background ' + motion.fast },
  avatar: { position: 'relative', width: 50, height: 50, borderRadius: 16, background: colors.avatar, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 16, fontWeight: 700, flex: 'none' },
  presence: { position: 'absolute', right: -2, bottom: -2, width: 14, height: 14, borderRadius: '50%', border: '2.5px solid ' + colors.ring },
  rowText: { flex: 1, display: 'flex', flexDirection: 'column', gap: 4, minWidth: 0 },
  rowTop: { display: 'flex', justifyContent: 'space-between', gap: 10 },
  name: { fontSize: 15, fontWeight: 700 },
  time: { fontSize: 11, fontWeight: 600 },
  rowBottom: { display: 'flex', justifyContent: 'space-between', gap: 10, alignItems: 'center' },
  preview: { fontSize: 13, fontWeight: 500, color: colors.textMuted, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' },
  unread: { minWidth: 20, height: 20, padding: '0 6px', borderRadius: 10, background: colors.accent, color: colors.white, fontSize: 11, fontWeight: 700, lineHeight: '20px', textAlign: 'center' },
  empty: { margin: '22px 22px 0', padding: 18, borderRadius: 20, background: colors.fillSofter, border: '1px dashed rgba(255,255,255,0.1)', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 9, textAlign: 'center' },
  emptyText: { margin: 0, fontSize: 13, fontWeight: 600, lineHeight: 1.5, color: colors.text60 },
};
