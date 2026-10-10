# AnyTime Chat — v1 UI

Offline-only v1: no Google sign-in, no phone or email verification. An account is
username + date of birth + gender, created on the device.

Stack: React + React Router v6, lucide-react icons, inline `React.CSSProperties` only.
Each screen file ends with its own uniquely-named `xxxStyles: Record<string, React.CSSProperties>`.

## Theming (light by default)
`theme/theme.ts` defines every value as a CSS custom property (`--at-*`) so the style
records stay static objects while the theme switches at runtime:

- `installTheme()` injects one stylesheet: `:root` (light) + `[data-theme="dark"]` + the four keyframes (`at-ringPulse`, `at-sweep`, `at-softIn`, `at-blink`).
- `getThemeMode()` / `applyThemeMode('light' | 'dark' | 'system')` — persisted in `localStorage['anytime.theme']`, default `light`. Called from `App` on mount and from Profile > Appearance.
- Import `colors`, `font`, `radius`, `space`, `shadow`, `motion` — never raw hex in screens.

## Files
- `api/client.ts` — all backend calls, base `http://localhost:8080/api` (`VITE_API_URL` overrides). Auth endpoints are gone; account endpoints are `GET/POST/PATCH /account` + `/account/username-available`.
- `components/Screen.tsx` — phone shell + status bar. `components/BottomNav.tsx` — Chats / Requests / [Nearby FAB] / Profile.
- Screens: Splash, Welcome, SetupAccount, BluetoothOff, Nearby, Chats, Chat, Profile.
- `components/Logo.tsx` — `LogoMark`, `LogoTile`, `Wordmark` ("Ping bubble" mark). Use these everywhere the brand appears.

## Splash
`/` renders `SplashScreen`: shows for at least 1.4s, calls `GET /account` and reads `useBluetooth()`, then routes no account → `/welcome`, BT off → `/bluetooth`, otherwise → `/nearby`.

## Bluetooth gate
v1 is Bluetooth-only, so `/nearby`, `/chats`, `/chats/:id` and `/profile` are wrapped in
`RequireBluetooth` (in `App.tsx`); with the radio off they redirect to `/bluetooth`, which has
no nav and no dismiss. `hooks/useBluetooth.ts` owns the state and talks to the native shell via
`window.AnyTimeBluetooth`, which Claude Code needs to implement on the Android side:

| member | Android |
| --- | --- |
| `getState()` | `BluetoothAdapter` state → `'on' \| 'off' \| 'unauthorized'` |
| `requestEnable()` | `ACTION_REQUEST_ENABLE` system dialog; resolves true if the user accepts |
| `openSettings()` | `ACTION_BLUETOOTH_SETTINGS` |
| `onStateChange(cb)` | `ACTION_STATE_CHANGED` receiver; returns an unsubscribe fn |
| `canPromptInApp` | `true` on Android, `false` on iOS (Settings-only) |

With no bridge present (plain browser dev) the hook reports `'on'` so the app stays usable.
Backend: `POST /bluetooth/ready` fires when the radio comes up (restart mesh, flush queue);
`GET /bluetooth/status` reports mesh state and queue depth.

Load Manrope once in `index.html`:
`<link href="https://fonts.googleapis.com/css2?family=Manrope:wght@400;500;600;700;800&display=swap" rel="stylesheet">`
