import React, { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Check, ChevronLeft, ShieldCheck } from 'lucide-react';

import Screen from '../components/Screen';
import { colors, motion, shadow } from '../theme/theme';
import { api, ApiError, type Gender } from '../api/client';

const MONTHS = ['January','February','March','April','May','June','July','August','September','October','November','December'];
const GENDERS: Gender[] = ['Male', 'Female', 'Other', 'Prefer not to say'];

// Mirrors core/domain/constants/RequestConstants.h — keep the two in step.
const MIN_USERNAME = 3;
const MAX_USERNAME = 20;
const MIN_AGE = 13;

const USERNAME_RE = new RegExp(`^[a-z0-9_]{${MIN_USERNAME},${MAX_USERNAME}}$`);

/**
 * Account creation. This single call generates the identity keypair — there is
 * no separate sign-in, because there is no account server to sign in to.
 *
 * AD-05: THERE IS NO USERNAME AVAILABILITY CHECK. With no server there is no
 * global registry to check against, so two people may both be "vinay". Identity
 * is the keypair; the UI disambiguates with the fingerprint suffix shown below
 * the field. Any "Taken"/"Available" affordance here would be a lie.
 */
export default function SetupAccountScreen() {
  const navigate = useNavigate();

  const [username, setUsername] = useState('');
  const [day, setDay] = useState('');
  const [month, setMonth] = useState('');
  const [year, setYear] = useState('');
  const [gender, setGender] = useState<Gender | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');

  const usernameValid = USERNAME_RE.test(username);
  const usernameTouched = username.length > 0;
  const age = ageFrom(day, month, year);
  const dobComplete = Boolean(day && month && year.length === 4);
  const ready = usernameValid && gender !== null && age !== null && age >= MIN_AGE;

  async function onCreate() {
    if (!ready || busy) return;
    setBusy(true);
    setError('');
    try {
      const dob = `${year}-${String(MONTHS.indexOf(month) + 1).padStart(2, '0')}-${day.padStart(2, '0')}`;
      await api.createAccount({ username, dob, gender: gender as Gender });
      navigate('/nearby', { replace: true });
    } catch (e) {
      setError(
        e instanceof ApiError && e.code === 'unreachable'
          ? 'Cannot reach the app core. Close AnyTime and open it again.'
          : e instanceof Error
            ? e.message
            : 'Could not create your account.',
      );
      setBusy(false);
    }
  }

  return (
    <Screen>
      <div style={setupStyles.header}>
        <button style={setupStyles.back} onClick={() => navigate(-1)} aria-label="Back">
          <ChevronLeft size={17} strokeWidth={2.1} />
        </button>
        <span style={setupStyles.headerLabel}>Create your account</span>
      </div>

      <div style={setupStyles.body}>
        <h2 style={setupStyles.title}>Pick a username</h2>
        <p style={setupStyles.sub}>This is what nearby people see.</p>

        <div style={setupStyles.fields}>
          <label style={setupStyles.field}>
            <span style={setupStyles.label}>USERNAME</span>
            <span style={{ ...setupStyles.input, ...(usernameValid ? setupStyles.inputFocus : null) }}>
              <span style={setupStyles.at}>@</span>
              <input
                value={username}
                onChange={(e) =>
                  setUsername(e.target.value.toLowerCase().replace(/[^a-z0-9_]/g, '').slice(0, MAX_USERNAME))
                }
                placeholder="username"
                autoCapitalize="none"
                autoCorrect="off"
                spellCheck={false}
                style={setupStyles.bare}
              />
              {usernameValid && (
                <span style={setupStyles.ok}>
                  <Check size={14} strokeWidth={2.6} />
                </span>
              )}
            </span>

            {/* Explains the discriminator BEFORE it appears, so seeing
                "@vinay·4f2a" on the Nearby screen later is not a surprise. */}
            <span style={setupStyles.helper}>
              {usernameTouched && !usernameValid
                ? `${MIN_USERNAME}–${MAX_USERNAME} characters — letters, numbers and _`
                : 'Names do not have to be unique. A short code is added so people can tell duplicates apart.'}
            </span>
          </label>

          <label style={setupStyles.field}>
            <span style={setupStyles.labelRow}>
              <span style={setupStyles.label}>DATE OF BIRTH</span>
              <span style={setupStyles.hint}>{MIN_AGE}+ only</span>
            </span>
            <span style={setupStyles.dobRow}>
              <input
                value={day}
                onChange={(e) => setDay(e.target.value.replace(/\D/g, '').slice(0, 2))}
                placeholder="DD"
                inputMode="numeric"
                style={{ ...setupStyles.dobCell, flex: 1 }}
              />
              <select value={month} onChange={(e) => setMonth(e.target.value)} style={{ ...setupStyles.dobCell, flex: 1.4 }}>
                <option value="">Month</option>
                {MONTHS.map((m) => (
                  <option key={m} value={m}>{m}</option>
                ))}
              </select>
              <input
                value={year}
                onChange={(e) => setYear(e.target.value.replace(/\D/g, '').slice(0, 4))}
                placeholder="YYYY"
                inputMode="numeric"
                style={{ ...setupStyles.dobCell, flex: 1.2 }}
              />
            </span>
            {dobComplete && age === null && (
              <span style={setupStyles.error}>That date does not exist.</span>
            )}
            {age !== null && age < MIN_AGE && (
              <span style={setupStyles.error}>You must be at least {MIN_AGE} to use AnyTime.</span>
            )}
          </label>

          <div style={setupStyles.field}>
            <span style={setupStyles.label}>GENDER</span>
            <div style={setupStyles.chips}>
              {GENDERS.map((g) => (
                <button
                  key={g}
                  onClick={() => setGender(g)}
                  style={{ ...setupStyles.chip, ...(gender === g ? setupStyles.chipActive : null) }}
                >
                  {g}
                </button>
              ))}
            </div>
          </div>

          <div style={setupStyles.note}>
            <ShieldCheck size={18} strokeWidth={2} color={colors.accent} />
            <p style={setupStyles.noteText}>
              Nothing is sent anywhere. Your account and its keys are created on this device only.
            </p>
          </div>

          {error && <span style={setupStyles.error}>{error}</span>}
        </div>
      </div>

      <div style={setupStyles.footer}>
        <button
          style={{ ...setupStyles.cta, ...(ready && !busy ? null : setupStyles.ctaDisabled) }}
          disabled={!ready || busy}
          onClick={onCreate}
        >
          {busy ? 'Creating your keys…' : 'Create account'}
        </button>
      </div>
    </Screen>
  );
}

/** Whole years, or null when the date is incomplete or impossible. */
function ageFrom(day: string, month: string, year: string): number | null {
  if (!day || !month || year.length !== 4) return null;

  const d = Number(day);
  const monthIndex = MONTHS.indexOf(month);
  const y = Number(year);
  const dob = new Date(y, monthIndex, d);

  // Rejects 31 February, which Date would silently roll into March.
  if (dob.getFullYear() !== y || dob.getMonth() !== monthIndex || dob.getDate() !== d) return null;

  const now = new Date();
  let a = now.getFullYear() - dob.getFullYear();
  const m = now.getMonth() - dob.getMonth();
  if (m < 0 || (m === 0 && now.getDate() < dob.getDate())) a--;
  return a;
}

const setupStyles: Record<string, React.CSSProperties> = {
  header: { display: 'flex', alignItems: 'center', gap: 14, padding: '18px 22px 0', flex: 'none' },
  back: { width: 38, height: 38, borderRadius: 12, border: '1px solid ' + colors.borderStrong, background: colors.surface, color: colors.text, display: 'flex', alignItems: 'center', justifyContent: 'center', cursor: 'pointer', transition: 'background ' + motion.fast },
  headerLabel: { fontSize: 13, fontWeight: 700, color: colors.text50 },
  body: { flex: 1, overflowY: 'auto', padding: '28px 28px 0' },
  title: { margin: 0, fontSize: 27, fontWeight: 800, lineHeight: 1.15, letterSpacing: '-0.02em' },
  sub: { margin: '9px 0 0', fontSize: 14, fontWeight: 500, lineHeight: 1.5, color: colors.textMuted },
  fields: { marginTop: 26, display: 'flex', flexDirection: 'column', gap: 18 },
  field: { display: 'flex', flexDirection: 'column', gap: 8 },
  labelRow: { display: 'flex', justifyContent: 'space-between', alignItems: 'baseline' },
  label: { fontSize: 12, fontWeight: 600, color: colors.text50, letterSpacing: '0.02em' },
  hint: { fontSize: 11, fontWeight: 600, color: colors.textFaint },
  helper: { fontSize: 11.5, fontWeight: 500, lineHeight: 1.5, color: colors.textFaint },
  input: { display: 'flex', alignItems: 'center', gap: 10, height: 54, padding: '0 16px', borderRadius: 14, background: colors.surface, border: '1px solid ' + colors.border, transition: 'border-color ' + motion.fast + ', box-shadow ' + motion.fast },
  inputFocus: { border: '1px solid rgba(68,83,246,0.55)', boxShadow: '0 0 0 3px rgba(68,83,246,0.12)' },
  at: { fontSize: 15, fontWeight: 600, color: colors.text35 },
  bare: { flex: 1, minWidth: 0, background: 'transparent', border: 0, outline: 'none', color: colors.text, fontSize: 15, fontWeight: 600, fontFamily: 'inherit' },
  ok: { display: 'flex', alignItems: 'center', color: colors.success, flex: 'none' },
  dobRow: { display: 'flex', gap: 10 },
  dobCell: { height: 54, borderRadius: 14, background: colors.surface, border: '1px solid ' + colors.border, color: colors.text, fontSize: 15, fontWeight: 600, fontFamily: 'inherit', textAlign: 'center', outline: 'none' },
  chips: { display: 'flex', flexWrap: 'wrap', gap: 9 },
  chip: { padding: '11px 17px', borderRadius: 13, background: colors.surface, border: '1px solid ' + colors.borderStrong, color: colors.text70, fontSize: 13.5, fontWeight: 600, fontFamily: 'inherit', cursor: 'pointer', transition: 'background ' + motion.fast + ', color ' + motion.fast },
  chipActive: { background: colors.accent, border: '1px solid ' + colors.accent, color: colors.white, fontWeight: 700 },
  note: { display: 'flex', gap: 12, padding: '14px 16px', borderRadius: 16, background: colors.accentSoft, border: '1px solid ' + colors.accentBorder },
  noteText: { margin: 0, fontSize: 12.5, fontWeight: 500, lineHeight: 1.55, color: colors.text60 },
  error: { fontSize: 12, fontWeight: 600, color: colors.danger },
  footer: { flex: 'none', padding: '16px 28px 30px', background: colors.fade },
  cta: { width: '100%', height: 56, border: 0, borderRadius: 16, background: colors.accent, color: colors.white, fontSize: 15.5, fontWeight: 700, fontFamily: 'inherit', cursor: 'pointer', boxShadow: shadow.accent, transition: 'background ' + motion.fast },
  ctaDisabled: { background: colors.fillSoft, color: colors.textFaint, boxShadow: 'none', cursor: 'not-allowed' },
};
