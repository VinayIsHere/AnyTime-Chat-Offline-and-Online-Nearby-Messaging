import React, { useState } from 'react';
import { Check, ShieldCheck } from 'lucide-react';
import { colors, motion } from '../theme/theme';
import { api, type ReportOutcome, type ReportReason } from '../api/client';

/** Mirrors the reasons the core accepts — see adapters/support/SupportLinks.h. */
const REASONS: Array<{ id: ReportReason; label: string }> = [
  { id: 'child_safety', label: 'Child safety concern' },
  { id: 'sexual_content', label: 'Nudity or sexual content' },
  { id: 'harassment', label: 'Harassment or threats' },
  { id: 'spam', label: 'Spam or scam' },
  { id: 'other', label: 'Something else' },
];

interface Props {
  peerId: string;
  handle: string;
  /** The sheet is finished with, whether or not anything was reported. */
  onClose: () => void;
  /** The person is now blocked. Fires before the user dismisses the sheet. */
  onReported: () => void;
}

/**
 * Reporting someone, from a chat or from a request.
 *
 * WHAT A REPORT IS HERE. AnyTime has no server and chats are end-to-end
 * encrypted, so there is no report to "submit" and nothing we could read if
 * there were. Reporting therefore does two things, and says so:
 *
 *   1. it BLOCKS the person, on this phone, at once — this needs no internet
 *      and is the half that actually protects the user;
 *   2. it opens the phone's mail app with a report to us already written: who,
 *      why, and both ids. The user adds what happened and presses Send.
 *
 * THE APP IS USED OFFLINE, which is why step 2 is worded the way it is. A mail
 * app keeps an unsent message in its outbox and sends it when the phone next has
 * a connection, so reporting works on a mountain as well as in a city — the
 * sheet just has to be honest that the email leaves later.
 *
 * The address is always shown on the last step. No mail app may be installed,
 * or the user may discard the draft and want to write later.
 */
export default function ReportSheet({ peerId, handle, onClose, onReported }: Props) {
  const [reason, setReason] = useState<ReportReason | null>(null);
  const [busy, setBusy] = useState(false);
  const [failed, setFailed] = useState(false);
  const [outcome, setOutcome] = useState<ReportOutcome | null>(null);

  async function submit() {
    if (!reason || busy) return;
    setBusy(true);
    setFailed(false);
    try {
      const result = await api.reportPeer(peerId, reason);
      setOutcome(result);
      onReported();
    } catch {
      setFailed(true);
    } finally {
      setBusy(false);
    }
  }

  return (
    // A tap outside closes it, as with the chat menu — but not mid-request,
    // where closing would hide whether the block went through.
    <div style={sheetStyles.scrim} onClick={busy ? undefined : onClose}>
      <div
        role="dialog"
        aria-modal="true"
        aria-label={'Report ' + handle}
        style={sheetStyles.sheet}
        onClick={(e) => e.stopPropagation()}
      >
        <span style={sheetStyles.grabber} />

        {outcome ? (
          <>
            <span style={sheetStyles.doneMark}>
              <ShieldCheck size={24} strokeWidth={2} color={colors.success} />
            </span>
            <h2 style={sheetStyles.title}>{handle} is blocked</h2>
            <p style={sheetStyles.body}>
              {outcome.emailOpened
                ? 'Your report is ready in your mail app. Press Send there to finish it. '
                  + 'With no internet right now, your mail app will send it once you are back online.'
                : 'No mail app opened on this phone. To finish your report, write to us at the '
                  + 'address below when you can.'}
            </p>
            <p style={sheetStyles.address}>{outcome.email}</p>
            <button style={sheetStyles.primary} onClick={onClose}>Done</button>
          </>
        ) : (
          <>
            <h2 style={sheetStyles.title}>Report {handle}</h2>
            <p style={sheetStyles.body}>What is wrong?</p>

            <div role="radiogroup" aria-label="Reason" style={sheetStyles.options}>
              {REASONS.map(({ id, label }) => (
                <button
                  key={id}
                  role="radio"
                  aria-checked={reason === id}
                  style={{ ...sheetStyles.option, ...(reason === id ? sheetStyles.optionOn : null) }}
                  onClick={() => setReason(id)}
                >
                  <span style={sheetStyles.optionLabel}>{label}</span>
                  {reason === id && <Check size={16} strokeWidth={2.6} color={colors.accent} />}
                </button>
              ))}
            </div>

            <p style={sheetStyles.note}>
              They are blocked straight away, even with no internet. The report itself is an email
              to our safety team: your mail app opens with it filled in, and sends it when you are
              online.
            </p>

            {failed && (
              <p style={sheetStyles.error} role="alert">Could not report just now. Please try again.</p>
            )}

            <div style={sheetStyles.actions}>
              <button style={sheetStyles.secondary} onClick={onClose} disabled={busy}>Cancel</button>
              <button
                style={{ ...sheetStyles.danger, opacity: reason ? 1 : 0.45 }}
                onClick={submit}
                disabled={!reason || busy}
              >
                {busy ? 'Reporting…' : 'Report and block'}
              </button>
            </div>
          </>
        )}
      </div>
    </div>
  );
}

const sheetStyles: Record<string, React.CSSProperties> = {
  scrim: { position: 'fixed', inset: 0, zIndex: 50, display: 'flex', alignItems: 'flex-end', justifyContent: 'center', background: 'rgba(12,12,20,0.42)', animation: 'at-fadeIn 0.18s ease both' },
  // Owns the bottom safe-area inset, like every other surface that reaches the
  // bottom edge. maxHeight + scroll keeps it usable on a short screen. The top
  // border is what separates it from the page in the dark theme, where the
  // sheet and the page behind it are the same colour.
  sheet: { width: '100%', maxWidth: 520, maxHeight: '88%', overflowY: 'auto', boxSizing: 'border-box', padding: '10px 20px 0', paddingBottom: 'calc(20px + env(safe-area-inset-bottom, 0px))', borderRadius: '24px 24px 0 0', borderTop: '1px solid ' + colors.borderStrong, background: colors.bg, color: colors.text, boxShadow: '0 -12px 40px rgba(18,18,26,0.18)', display: 'flex', flexDirection: 'column', animation: 'at-sheetIn 0.26s cubic-bezier(0.22,0.61,0.36,1) both' },
  grabber: { alignSelf: 'center', width: 38, height: 4, borderRadius: 2, background: colors.borderStrong, marginBottom: 16, flex: 'none' },
  title: { margin: 0, fontSize: 19, fontWeight: 800, letterSpacing: '-0.02em' },
  body: { margin: '8px 0 0', fontSize: 13.5, fontWeight: 500, lineHeight: 1.55, color: colors.text60 },
  options: { marginTop: 14, display: 'flex', flexDirection: 'column', gap: 8 },
  option: { display: 'flex', alignItems: 'center', gap: 10, minHeight: 48, padding: '0 15px', borderRadius: 14, border: '1px solid ' + colors.border, background: colors.surface, color: colors.text, fontFamily: 'inherit', textAlign: 'left', cursor: 'pointer', transition: 'border-color ' + motion.fast + ', background ' + motion.fast },
  optionOn: { borderColor: colors.accentBorder, background: colors.accentSoft },
  optionLabel: { flex: 1, fontSize: 14, fontWeight: 700 },
  note: { margin: '14px 2px 0', fontSize: 12, fontWeight: 500, lineHeight: 1.55, color: colors.text50 },
  error: { margin: '12px 0 0', padding: '10px 13px', borderRadius: 12, background: colors.fillSoft, fontSize: 12.5, fontWeight: 600, color: colors.danger },
  actions: { marginTop: 18, display: 'flex', gap: 10 },
  secondary: { flex: 1, height: 48, borderRadius: 14, border: '1px solid ' + colors.borderStrong, background: 'transparent', color: colors.text70, fontSize: 14, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer' },
  danger: { flex: 1.5, height: 48, borderRadius: 14, border: 0, background: colors.danger, color: colors.white, fontSize: 14, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer', transition: 'opacity ' + motion.fast },
  primary: { marginTop: 18, height: 48, borderRadius: 14, border: 0, background: colors.accent, color: colors.white, fontSize: 14, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer' },
  doneMark: { width: 52, height: 52, borderRadius: 18, background: colors.fillSoft, display: 'flex', alignItems: 'center', justifyContent: 'center', marginBottom: 14, flex: 'none' },
  // Selectable on purpose: this is the one piece of text on the sheet someone
  // may need to copy.
  address: { margin: '12px 0 0', padding: '11px 14px', borderRadius: 12, background: colors.fillSoft, fontSize: 13.5, fontWeight: 700, userSelect: 'text', WebkitUserSelect: 'text', wordBreak: 'break-all' },
};
