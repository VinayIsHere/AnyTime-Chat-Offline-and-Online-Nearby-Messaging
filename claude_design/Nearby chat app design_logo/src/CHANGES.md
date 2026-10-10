# AnyTime Chat — UI handoff (v1, Bluetooth-only)

Context for Claude Code. This folder is the complete UI layer. It talks to a local backend over HTTP and to the native shell through `window.AnyTimeBluetooth`. Everything below is decided; build against it.

## Stack rules (keep these)
- React + React Router v6, `lucide-react` icons.
- Styling is inline `React.CSSProperties` only. No CSS files, Tailwind or styled-components. Each file ends with a uniquely named `const xxxStyles: Record<string, React.CSSProperties>`.
- All tokens live in `theme/theme.ts`. Screens never use raw hex values, except the fixed brand accent `#4453F6`, which comes through `colors.accent`.
- Font: Manrope (400–800). Load it once in `index.html`:
  `<link href="https://fonts.googleapis.com/css2?family=Manrope:wght@400;500;600;700;800&display=swap" rel="stylesheet">`

## File map
```
src/
  App.tsx                      routes + RequireBluetooth gate
  theme/theme.ts               tokens (CSS vars), light/dark themes, keyframes, theme mode helpers
  api/client.ts                every backend call (base http://localhost:8080/api, VITE_API_URL overrides)
  hooks/useBluetooth.ts        radio state + native bridge contract
  components/Screen.tsx        phone shell + status bar
  components/BottomNav.tsx     Chats / Requests / [Nearby FAB] / Profile
  components/Logo.tsx          LogoMark, LogoTile, Wordmark (Ping bubble)
  screens/SplashScreen.tsx     /
  screens/WelcomeScreen.tsx    /welcome
  screens/SetupAccountScreen.tsx /setup
  screens/BluetoothOffScreen.tsx /bluetooth
  screens/NearbyScreen.tsx     /nearby
  screens/ChatsScreen.tsx      /chats
  screens/ChatScreen.tsx       /chats/:chatId
  screens/ProfileScreen.tsx    /profile
```

## App flow
```
/ (Splash, >= 1.4s)
 ├─ no local account ──> /welcome ──> /setup ──> /nearby
 ├─ account, BT off ───> /bluetooth  (hard gate, closes itself when BT turns on)
 └─ account, BT on ────> /nearby
```
`/nearby`, `/chats`, `/chats/:chatId` and `/profile` are wrapped in `RequireBluetooth`. If the radio turns off at any point, the user is redirected to `/bluetooth`.

---

## Changes and reasons

### 1. Brand: Ping bubble logo (final)
- **What:** `components/Logo.tsx` exports `LogoMark` (bare mark), `LogoTile` (app-icon tile: accent square with a white bubble) and `Wordmark` (mark + "AnyTime"). The mark is a chat bubble carrying a dot and two signal arcs. It is drawn as inline SVG because lucide has no brand marks.
- **Where it's used:** the splash screen (104px tile) and the welcome screen (76px tile). It replaced the generic Bluetooth glyph used earlier.
- **Why:** of the three concepts explored, this one reads as both "chat" and "nearby" at icon size. Its arcs match the Nearby FAB icon, so the brand and the main action look alike.
- **Todo for Claude Code:** export the tile as the Android adaptive icon (foreground: white bubble; background: `#4453F6`) and as the favicon. Use the geometry in `Logo.tsx`: viewBox `0 0 64 64`, `BUBBLE` and `ARCS` paths.

### 2. Splash screen
- **What:** `SplashScreen.tsx` is the `/` route. It shows the logo tile with two pulsing rings, the name and tagline, and a thin progress bar with "WORKS WITHOUT INTERNET" below.
- **Logic:** it waits at least 1.4s (`MIN_SHOW_MS`) while it calls `GET /account` and reads `useBluetooth()`. It then routes as in the flow above, using `replace: true` so the back button never returns to the splash.
- **Why:** the app needs to know whether an account exists and whether the radio is on before it can choose a first screen. The minimum duration stops a flash on fast devices.

### 3. Onboarding: username + DOB + gender only
- **What:** removed Google sign-in, phone and email entry, and OTP verification (`VerifyScreen` and `CreateProfileScreen` are deleted). `SetupAccountScreen` collects a username (3–20 chars, `a-z 0-9 _`, live availability check), a date of birth (13+ age gate, enforced client-side), and a gender (Male / Female / Other / Prefer not to say). `WelcomeScreen` has a single "Set up username" CTA.
- **Why:** v1 is offline-only, and verification requires a network. The account is created on the device.
- **Backend:** `GET /account/username-available?u=`, `POST /account` `{ username, dob: 'YYYY-MM-DD', gender }`, `GET /account`, `PATCH /account`.

### 4. Bluetooth hard gate
- **What:** `BluetoothOffScreen.tsx` has no nav and no dismiss. It has two states, `off` and `unauthorized`, plus a "denied" message if the user declines the system prompt.
  - On Android (`canPromptInApp: true`), the primary button "Turn on Bluetooth" calls `requestEnable()`, which shows the system "Allow AnyTime to turn on Bluetooth?" dialog. The secondary button opens Settings.
  - On iOS (`canPromptInApp: false`), the primary button is "Open Bluetooth settings".
- **Why:** every v1 feature depends on Bluetooth, so partial use with the radio off would only show broken states.
- **Native bridge to implement** (`window.AnyTimeBluetooth`, typed in `hooks/useBluetooth.ts`):

| Member | Android implementation |
| --- | --- |
| `getState(): Promise<'on'\|'off'\|'unauthorized'>` | `BluetoothAdapter.isEnabled` + `BLUETOOTH_CONNECT`/`BLUETOOTH_SCAN` permission check |
| `requestEnable(): Promise<boolean>` | `BluetoothAdapter.ACTION_REQUEST_ENABLE`; resolve `true` on RESULT_OK |
| `openSettings(): Promise<void>` | `Settings.ACTION_BLUETOOTH_SETTINGS` |
| `onStateChange(cb): () => void` | receiver for `BluetoothAdapter.ACTION_STATE_CHANGED`; return unsubscribe |
| `canPromptInApp: boolean` | `true` on Android, `false` on iOS |

  With no bridge present (plain browser dev), the hook reports `'on'` so the app stays usable.
- **Backend:** `POST /bluetooth/ready` fires each time the radio comes up, so the backend can restart the mesh and flush queued messages. `GET /bluetooth/status` returns `{ meshActive, queued }`.

### 5. Theme: light by default, dark optional
- **What:** `theme.ts` defines every themeable value as a CSS custom property (`--at-*`). `installTheme()` injects one stylesheet containing the `:root` light values, the `[data-theme="dark"]` values and all keyframes. `applyThemeMode('light'|'dark'|'system')` sets `data-theme` on `<html>` and persists the choice in `localStorage['anytime.theme']`. The default is `light`.
- **UI:** Profile has an "Appearance" segmented control (Light / Dark / System).
- **Why:** the style records have to stay static objects, as the inline-only rule requires. CSS variables let one set of records serve both themes with no re-render logic. Keyframes go in the same injected sheet because inline styles can't declare them.
- **Keyframes available:** `at-ringPulse`, `at-sweep`, `at-softIn`, `at-blink`, `at-spin`, `at-logoIn`.

### 6. Main shell and screens
- **Bottom nav:** Chats, Requests (with a badge), a raised Nearby FAB centered in the empty third column at `left: 62.5%`, and Profile. **Why:** an earlier centered FAB covered the Requests tab.
- **Nearby:** a radar sweep with pulsing rings and avatars appearing for found users, plus a list showing distance and signal strength. The first item's "Say hi" button is the primary style. Calls: `POST /nearby/scan`, then poll `GET /nearby/:scanId` every 2.5s. "Say hi" calls `POST /chats` `{ userId }` and opens `/chats/:chatId`.
- **Chats:** search, presence dots, unread badges, and an out-of-range row showing a queued-message count. Call: `GET /chats`.
- **Chat:** a header showing distance and connection state, "Connected over Bluetooth · No internet" pill, optimistic sends with `queued`/`sent`/`delivered` receipts, and a typing indicator. Calls: `GET /chats/:id/messages`, `POST /chats/:id/messages` `{ body }`. Outgoing bubbles use `width: fit-content` so short messages don't stretch to the width of the receipt line.
- **Profile:** avatar, username, age and gender. Controls: a discoverable toggle (`PATCH /account { discoverable }`, rolled back on failure), Appearance, "Accept messages from" and "Blocked users".
- "Say hi" buttons use `whiteSpace: 'nowrap'` and `flex: 'none'` so the label never wraps.

## Not built yet (expected next)
- `/requests` screen. The nav tab links to it, but the route doesn't exist yet.
- Edit profile, the "Accept messages from" picker, and the blocked users list (the rows are present but not wired up).
- v2 online mode.
