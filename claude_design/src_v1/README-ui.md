# AnyTime Chat — v1 UI

Offline-only v1: no Google sign-in, no phone or email verification. An account is
username + date of birth + gender, created on the device.

Stack: React + React Router v6, lucide-react icons, inline \`React.CSSProperties\` only.
Each screen file ends with its own uniquely-named \`xxxStyles: Record<string, React.CSSProperties>\`.

## Theming (light by default)
\`theme/theme.ts\` defines every value as a CSS custom property (\`--at-*\`) so the style
records stay static objects while the theme switches at runtime:

- \`installTheme()\` injects one stylesheet: \`:root\` (light) + \`[data-theme="dark"]\` + the four keyframes (\`at-ringPulse\`, \`at-sweep\`, \`at-softIn\`, \`at-blink\`).
- \`getThemeMode()\` / \`applyThemeMode('light' | 'dark' | 'system')\` — persisted in \`localStorage['anytime.theme']\`, default \`light\`. Called from \`App\` on mount and from Profile > Appearance.
- Import \`colors\`, \`font\`, \`radius\`, \`space\`, \`shadow\`, \`motion\` — never raw hex in screens.

## Files
- \`api/client.ts\` — all backend calls, base \`http://localhost:8080/api\` (\`VITE_API_URL\` overrides). Auth endpoints are gone; account endpoints are \`GET/POST/PATCH /account\` + \`/account/username-available\`.
- \`components/Screen.tsx\` — phone shell + status bar. \`components/BottomNav.tsx\` — Chats / Requests / [Nearby FAB] / Profile.
- Screens: Welcome, SetupAccount, Nearby, Chats, Chat, Profile.

Load Manrope once in \`index.html\`:
\`<link href="https://fonts.googleapis.com/css2?family=Manrope:wght@400;500;600;700;800&display=swap" rel="stylesheet">\`
