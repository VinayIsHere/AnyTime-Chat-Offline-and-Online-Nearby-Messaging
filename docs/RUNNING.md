# Running AnyTime Chat

Verified against Qt 6.11.0 / mingw 13.1 on Windows, 2026-09-06.

---

## What works today

Sign-in and Nearby, end to end, against the real C++ core:

- Account creation generates a keypair, derives the `PeerId`, and builds the
  `@vinay·731a` handle
- Peers discover each other by exchanging signed `ANNOUNCE` packets
- Discovery results push to the UI over SSE — no polling
- The local API is gated by a per-launch 256-bit secret path prefix

Not yet: messaging, real cryptography, the BLE radio, SQLite. See "What is fake"
below — it matters.

---

## Desktop (Windows)

The desktop build is **headless**: it runs the core and the local API server and
prints a URL for you to open in a browser. Qt WebEngine is not part of the
standard desktop kit, and browser iteration is a faster loop than rebuilding a
native shell.

### Build

```bash
# 1. Build the UI bundle (the C++ server serves dist/)
cd ui
npm install
npm run build

# 2. Configure and build the app
cd ..
cmake -S . -B build/desktop -G Ninja \
  -DCMAKE_MAKE_PROGRAM="C:/Qt/Tools/Ninja/ninja.exe" \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.0/mingw_64" \
  -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe" \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build/desktop
```

In Qt Creator, just open `CMakeLists.txt` and pick the **Desktop Qt 6.11.0
MinGW 64-bit** kit.

### Run

`C:\Qt\6.11.0\mingw_64\bin` and `C:\Qt\Tools\mingw1310_64\bin` must be on `PATH`
so the Qt DLLs resolve.

```bash
build/desktop/anytime.exe
```

It prints:

```
  ┌──────────────────────────────────────────────────────────────
  │  AnyTime Chat 0.1
  │
  │  Open this in a browser:
  │  http://127.0.0.1:64645/656acb869cc7...af2a67/
  │
  │  Second instance:  anytime --instance b
  │
  │  URL also written to:
  │  C:/Users/<you>/AppData/Roaming/AnyTime/AnyTime Chat/current-url.txt
  └──────────────────────────────────────────────────────────────
```

**The token changes every launch**, so copy the URL fresh each time — or read it
from `current-url.txt`, which is there precisely so this is scriptable.

### Two instances, so Nearby has something to find

```bash
build/desktop/anytime.exe                  # instance A
build/desktop/anytime.exe --instance b     # instance B, separate identity
```

`--instance` gives a separate data directory, so the two get different keypairs
and see each other as strangers. Open both URLs in two browser tabs, create an
account in each, and each shows the other on its Nearby radar within a few
seconds.

That is the loopback transport (AD-07) doing exactly what it exists for: the
whole discovery path exercised with no radio on a machine that has no BLE
peripheral support at all.

### Resetting

```bash
rm -rf "$LOCALAPPDATA/../Roaming/AnyTime"   # accounts and identities
rm -rf "$TEMP/anytime-loopback"             # peer rendezvous files
```

---

## UI only, no C++ core

For pure front-end work, the React app runs standalone against a `localStorage`
mock of the account endpoints:

```bash
cd ui
npm run dev        # http://localhost:5173
```

Nearby stays empty here — there is no core and no transport. Use the desktop
build for anything involving peers.

Replay the welcome flow from the browser console:

```js
localStorage.removeItem('anytime.devmock.account'); location.reload()
```

---

## Android

**Not yet buildable.** Still missing:

- `platform/android/AndroidManifest.xml` and `build.gradle`
- A step to copy `ui/dist/` into `platform/android/assets/www`
- `BleGattTransport` + the JNI bridge onto `BleRadio.java`

The Qt/WebView shell path in `app/main.cpp` is written and compiles under
`ANYTIME_USE_WEBVIEW`; it just has no packaging around it. That is the next
milestone.

---

## Verifying by hand

```bash
URL=$(cat "$APPDATA/AnyTime/AnyTime Chat/current-url.txt")

curl -s "${URL}api/account"                     # 404 before setup
curl -s -X POST -H "Content-Type: application/json" \
     -d '{"username":"vinay","dob":"1998-04-12","gender":"Male"}' \
     "${URL}api/account"

curl -s -X POST "${URL}api/discovery"
curl -s "${URL}api/discovery/peers"
curl -s -N "${URL}api/events"                   # live SSE stream
```

Security gates, all confirmed working:

| Request | Result |
|---|---|
| No token in path | `404`, no hint a server exists |
| Wrong token | `404` |
| `DELETE /api/account` | `405`, not a misleading 404 |
| Unknown endpoint | `404 not_found` |
| Non-loopback peer | connection aborted |

---

## What is fake, and why it matters

Two components are deliberate placeholders. Both are loud about it in their own
headers, and both are replaced without touching anything above them.

### `DevIdentityStore` — NOT REAL CRYPTOGRAPHY

Generates random bytes and calls them a keypair. "Signatures" are a keyed hash
that **anyone holding the public key can forge**, so announce authenticity is
currently decorative. Keys sit unencrypted on disk.

The `PeerId` derivation — `SHA-256(publicKey)` truncated to 8 bytes — *is* the
real scheme, which is why handles and the fingerprint discriminator behave
correctly today.

A release build including this file fails to compile, by design.

### `SettingsStore` — account only

The account persists to an INI file. Peers, contacts and requests are RAM-only,
which is correct for peers (they expire after 60 seconds anyway) and temporary
for the rest. SQLite lands with messaging.

### `NullSessionFactory`

Every call fails with a clear error rather than pretending to encrypt. A
passthrough "session" would let plaintext flow while looking encrypted — the
worst failure mode available — so it fails loudly instead. Discovery does not
need sessions: announces are signed and cleartext by design.
