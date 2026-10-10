import React, { useEffect, useState } from 'react';
import { Bluetooth, Lock, ShieldCheck, ChevronRight, Sun, Moon, Monitor, Check } from 'lucide-react';
import Screen from '../components/Screen';
import BottomNav from '../components/BottomNav';
import { colors, motion, shadow, applyThemeMode, getThemeMode, ThemeMode } from '../theme/theme';
import { api, ApiError, type AcceptFrom, type Account, type BlockedPeer } from '../api/client';
import { isValidUsername, sanitizeUsername, USERNAME_HINT } from '../config/username';

const MODES: Array<{ id: ThemeMode; label: string; Icon: typeof Sun }> = [
  { id: 'light', label: 'Light', Icon: Sun },
  { id: 'dark', label: 'Dark', Icon: Moon },
  { id: 'system', label: 'System', Icon: Monitor },
];

const ACCEPT_OPTIONS: Array<{ id: AcceptFrom; label: string }> = [
  { id: 'everyone', label: 'Everyone nearby' },
  { id: 'nobody', label: 'No one' },
];

type Section = 'acceptFrom' | 'blocked' | null;

export default function ProfileScreen() {
  // Null until the core answers. This used to start from a hard-coded "vinay,
  // 25, Male" so every user saw someone else's profile flash up first.
  const [account, setAccount] = useState<Account | null>(null);
  const [loadFailed, setLoadFailed] = useState(false);
  const [blocked, setBlocked] = useState<BlockedPeer[] | null>(null);
  const [mode, setMode] = useState<ThemeMode>(getThemeMode());
  const [open, setOpen] = useState<Section>(null);
  const [error, setError] = useState('');

  const [editing, setEditing] = useState(false);
  const [draftName, setDraftName] = useState('');
  const [saving, setSaving] = useState(false);

  function load() {
    setLoadFailed(false);
    api.getAccount().then(setAccount).catch(() => setLoadFailed(true));
    api.getBlocked().then(setBlocked).catch(() => setBlocked(null));
  }

  useEffect(load, []);

  function pickMode(next: ThemeMode) {
    setMode(next);
    applyThemeMode(next);
  }

  /**
   * Optimistic, with a real rollback. The core answers with the account as
   * stored, so that — not the local guess — is what the screen ends up showing.
   */
  async function patch(change: Partial<Pick<Account, 'discoverable' | 'acceptFrom'>>) {
    if (!account) return;
    const before = account;
    setError('');
    setAccount({ ...account, ...change });
    try {
      setAccount(await api.updateAccount(change));
    } catch {
      setAccount(before);
      setError('Could not save that change.');
    }
  }

  function startEditing() {
    if (!account) return;
    setDraftName(account.username);
    setError('');
    setEditing(true);
  }

  async function saveName() {
    if (!account || !isValidUsername(draftName)) return;
    if (draftName === account.username) {
      setEditing(false);
      return;
    }
    setSaving(true);
    setError('');
    try {
      setAccount(await api.updateAccount({ username: draftName }));
      setEditing(false);
    } catch (e) {
      setError(e instanceof ApiError && e.status === 400 ? USERNAME_HINT : 'Could not save your name.');
    } finally {
      setSaving(false);
    }
  }

  async function unblock(peer: BlockedPeer) {
    setError('');
    try {
      await api.unblockPeer(peer.peerId);
      setBlocked((list) => (list ? list.filter((b) => b.peerId !== peer.peerId) : list));
    } catch {
      setError('Could not unblock ' + peer.handle + '.');
    }
  }

  const toggle = (section: Exclude<Section, null>) => setOpen((o) => (o === section ? null : section));

  if (!account) {
    return (
      <Screen>
        <div style={profileStyles.glow} />
        <div style={profileStyles.head}>
          {loadFailed ? (
            <div style={profileStyles.failed}>
              <p style={profileStyles.failedText}>Could not load your profile.</p>
              <button style={profileStyles.edit} onClick={load}>Try again</button>
            </div>
          ) : (
            // Shapes only, no words: nothing here can be mistaken for real data.
            <span style={{ ...profileStyles.avatar, ...profileStyles.skeleton }} aria-label="Loading profile" />
          )}
        </div>
        <BottomNav />
      </Screen>
    );
  }

  const nameValid = isValidUsername(draftName);
  const acceptLabel = ACCEPT_OPTIONS.find((o) => o.id === account.acceptFrom)?.label ?? 'Everyone nearby';
  const blockedLabel =
    blocked === null ? '—' : blocked.length === 0 ? 'None' : blocked.length === 1 ? '1 person' : blocked.length + ' people';

  return (
    <Screen>
      <div style={profileStyles.glow} />
      <div style={profileStyles.head}>
        <span style={profileStyles.avatar}>{account.username.charAt(0).toUpperCase()}</span>

        {editing ? (
          <div style={profileStyles.editor}>
            <span style={{ ...profileStyles.input, ...(nameValid ? profileStyles.inputOk : null) }}>
              <span style={profileStyles.at}>@</span>
              <input
                value={draftName}
                onChange={(e) => setDraftName(sanitizeUsername(e.target.value))}
                onKeyDown={(e) => { if (e.key === 'Enter') void saveName(); }}
                aria-label="Username"
                autoFocus
                autoCapitalize="none"
                autoCorrect="off"
                spellCheck={false}
                style={profileStyles.bare}
              />
            </span>
            {/* The handle keeps its code: renaming changes the label, never the
                identity, so contacts still see who this is. */}
            <span style={profileStyles.helper}>
              {nameValid ? 'Your code stays the same, so contacts still know it is you.' : USERNAME_HINT}
            </span>
            <div style={profileStyles.editorActions}>
              <button style={profileStyles.secondary} onClick={() => setEditing(false)} disabled={saving}>
                Cancel
              </button>
              <button
                style={{ ...profileStyles.primary, opacity: nameValid ? 1 : 0.5 }}
                onClick={saveName}
                disabled={!nameValid || saving}
              >
                {saving ? 'Saving…' : 'Save'}
              </button>
            </div>
          </div>
        ) : (
          <>
            <h2 style={profileStyles.name}>{account.handle}</h2>
            <p style={profileStyles.meta}>{yearsSince(account.dob)} · {account.gender} · On this device</p>
            <button style={profileStyles.edit} onClick={startEditing}>Edit profile</button>
          </>
        )}
      </div>

      {error && <div style={profileStyles.error} role="alert">{error}</div>}

      <div style={profileStyles.list}>
        <div style={profileStyles.row}>
          <Bluetooth size={19} strokeWidth={2} color={colors.accent} />
          <span style={profileStyles.rowText}>
            <span style={profileStyles.rowTitle}>Discoverable nearby</span>
            <span style={profileStyles.rowSub}>Others can see @{account.username} over Bluetooth</span>
          </span>
          <button
            style={{ ...profileStyles.toggle, background: account.discoverable ? colors.accent : colors.borderStrong, justifyContent: account.discoverable ? 'flex-end' : 'flex-start' }}
            onClick={() => patch({ discoverable: !account.discoverable })}
            aria-pressed={account.discoverable}
            aria-label="Discoverable nearby"
          >
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

        <div style={profileStyles.card}>
          <button style={profileStyles.rowButton} onClick={() => toggle('acceptFrom')} aria-expanded={open === 'acceptFrom'}>
            <Lock size={19} strokeWidth={2} color={colors.text60} />
            <span style={profileStyles.rowText}>
              <span style={profileStyles.rowTitle}>Accept messages from</span>
              <span style={profileStyles.rowSub}>{acceptLabel}</span>
            </span>
            <ChevronRight size={16} strokeWidth={2.2} color={colors.text35} style={chevron(open === 'acceptFrom')} />
          </button>
          {open === 'acceptFrom' && (
            <div style={profileStyles.panel} role="radiogroup" aria-label="Accept messages from">
              {ACCEPT_OPTIONS.map(({ id, label }) => (
                <button
                  key={id}
                  role="radio"
                  aria-checked={account.acceptFrom === id}
                  style={profileStyles.option}
                  onClick={() => { if (account.acceptFrom !== id) void patch({ acceptFrom: id }); }}
                >
                  <span style={profileStyles.rowText}>
                    <span style={profileStyles.optionTitle}>{label}</span>
                    <span style={profileStyles.rowSub}>
                      {id === 'everyone'
                        ? 'Anyone in range can send you a request.'
                        : 'New requests are dropped. Existing chats keep working.'}
                    </span>
                  </span>
                  {account.acceptFrom === id && <Check size={16} strokeWidth={2.6} color={colors.accent} />}
                </button>
              ))}
            </div>
          )}
        </div>

        <div style={profileStyles.card}>
          <button style={profileStyles.rowButton} onClick={() => toggle('blocked')} aria-expanded={open === 'blocked'}>
            <ShieldCheck size={19} strokeWidth={2} color={colors.text60} />
            <span style={profileStyles.rowText}>
              <span style={profileStyles.rowTitle}>Blocked users</span>
              <span style={profileStyles.rowSub}>{blockedLabel}</span>
            </span>
            <ChevronRight size={16} strokeWidth={2.2} color={colors.text35} style={chevron(open === 'blocked')} />
          </button>
          {open === 'blocked' && (
            <div style={profileStyles.panel}>
              {blocked === null ? (
                <p style={profileStyles.panelNote}>Could not load the block list.</p>
              ) : blocked.length === 0 ? (
                <p style={profileStyles.panelNote}>
                  Nobody. People you block can&rsquo;t see you nearby or send you anything.
                </p>
              ) : (
                blocked.map((b) => (
                  <div key={b.peerId} style={profileStyles.blockedRow}>
                    <span style={profileStyles.blockedAvatar}>{(b.nickname.charAt(0) || '?').toUpperCase()}</span>
                    <span style={profileStyles.rowText}>
                      <span style={profileStyles.optionTitle}>{b.handle}</span>
                      {b.at && <span style={profileStyles.rowSub}>Blocked {b.at === 'now' ? 'just now' : b.at + ' ago'}</span>}
                    </span>
                    <button style={profileStyles.unblock} onClick={() => unblock(b)} aria-label={'Unblock ' + b.handle}>
                      Unblock
                    </button>
                  </div>
                ))
              )}
            </div>
          )}
        </div>
      </div>

      <BottomNav />
    </Screen>
  );
}

function chevron(openNow: boolean): React.CSSProperties {
  return { flex: 'none', transform: openNow ? 'rotate(90deg)' : 'none', transition: 'transform ' + motion.fast };
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
  skeleton: { background: colors.fillSoft, boxShadow: 'none' },
  failed: { marginTop: 60, display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 4 },
  failedText: { margin: 0, fontSize: 14, fontWeight: 600, color: colors.text60 },
  name: { margin: '14px 0 0', fontSize: 22, fontWeight: 800, letterSpacing: '-0.02em' },
  meta: { margin: '6px 0 0', fontSize: 13, fontWeight: 500, color: colors.text50 },
  edit: { marginTop: 14, height: 42, padding: '0 20px', borderRadius: 14, border: '1px solid ' + colors.borderStrong, background: colors.surface, color: colors.text, fontSize: 13, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer', transition: 'background ' + motion.fast },

  editor: { width: '100%', maxWidth: 320, marginTop: 16, display: 'flex', flexDirection: 'column', gap: 8, textAlign: 'left' },
  input: { display: 'flex', alignItems: 'center', gap: 4, height: 50, padding: '0 16px', borderRadius: 14, background: colors.surface, border: '1px solid ' + colors.borderStrong, transition: 'border-color ' + motion.fast },
  inputOk: { borderColor: colors.accentBorder },
  at: { fontSize: 15, fontWeight: 700, color: colors.textFaint },
  bare: { flex: 1, minWidth: 0, background: 'transparent', border: 0, outline: 'none', color: colors.text, fontSize: 15, fontWeight: 600, fontFamily: 'inherit' },
  helper: { fontSize: 11.5, fontWeight: 500, lineHeight: 1.45, color: colors.text50, padding: '0 2px' },
  editorActions: { display: 'flex', gap: 8, marginTop: 4 },
  secondary: { flex: 1, height: 42, borderRadius: 13, border: '1px solid ' + colors.borderStrong, background: 'transparent', color: colors.text70, fontSize: 13.5, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer' },
  primary: { flex: 1.4, height: 42, borderRadius: 13, border: 0, background: colors.accent, color: colors.white, fontSize: 13.5, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer', transition: 'opacity ' + motion.fast },

  error: { flex: 'none', margin: '14px 20px 0', padding: '10px 13px', borderRadius: 12, background: colors.fillSoft, fontSize: 12.5, fontWeight: 600, color: colors.danger },

  // Bottom padding clears the NEARBY button, which overhangs the nav bar's top
  // edge — without it the last card sits half under the FAB.
  list: { flex: 1, overflowY: 'auto', padding: '22px 20px 28px', display: 'flex', flexDirection: 'column', gap: 10 },
  row: { display: 'flex', alignItems: 'center', gap: 13, padding: '15px 16px', borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border },
  card: { borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border, overflow: 'hidden', flex: 'none' },
  rowButton: { width: '100%', display: 'flex', alignItems: 'center', gap: 13, padding: '15px 16px', border: 0, background: 'transparent', color: colors.text, fontFamily: 'inherit', textAlign: 'left', cursor: 'pointer' },
  rowText: { flex: 1, display: 'flex', flexDirection: 'column', gap: 2, minWidth: 0 },
  rowTitle: { fontSize: 14, fontWeight: 700 },
  rowSub: { fontSize: 11.5, fontWeight: 500, color: colors.text50 },
  panel: { borderTop: '1px solid ' + colors.border, padding: 6, display: 'flex', flexDirection: 'column', gap: 2, animation: 'at-softIn 0.2s ease both' },
  panelNote: { margin: 0, padding: '10px 12px', fontSize: 12.5, fontWeight: 500, lineHeight: 1.5, color: colors.text50 },
  option: { display: 'flex', alignItems: 'center', gap: 12, padding: '11px 12px', border: 0, borderRadius: 12, background: 'transparent', color: colors.text, fontFamily: 'inherit', textAlign: 'left', cursor: 'pointer' },
  optionTitle: { fontSize: 13.5, fontWeight: 700 },
  blockedRow: { display: 'flex', alignItems: 'center', gap: 12, padding: '9px 10px' },
  blockedAvatar: { width: 34, height: 34, borderRadius: 11, background: colors.avatar, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 13, fontWeight: 700, flex: 'none' },
  unblock: { flex: 'none', height: 34, padding: '0 14px', borderRadius: 11, border: '1px solid ' + colors.borderStrong, background: 'transparent', color: colors.text, fontSize: 12.5, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer' },

  toggle: { width: 46, height: 27, borderRadius: 14, border: 0, padding: 3, display: 'flex', alignItems: 'center', flex: 'none', cursor: 'pointer', transition: 'background ' + motion.base },
  knob: { width: 21, height: 21, borderRadius: '50%', background: colors.white, display: 'block' },
  appearance: { padding: '15px 16px', borderRadius: 18, background: colors.surface, border: '1px solid ' + colors.border, display: 'flex', flexDirection: 'column', gap: 13 },
  appearanceHead: { display: 'flex', alignItems: 'center', gap: 13 },
  segmented: { display: 'flex', gap: 6, padding: 4, borderRadius: 14, background: colors.fillSoft },
  segment: { flex: 1, height: 38, borderRadius: 11, border: 0, background: 'transparent', color: colors.text50, display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 7, fontSize: 12.5, fontWeight: 600, fontFamily: 'inherit', whiteSpace: 'nowrap', cursor: 'pointer', transition: 'background ' + motion.fast + ', color ' + motion.fast },
  segmentActive: { background: colors.surface, color: colors.text, fontWeight: 700, boxShadow: '0 1px 3px rgba(18,18,26,0.12)' },
};
