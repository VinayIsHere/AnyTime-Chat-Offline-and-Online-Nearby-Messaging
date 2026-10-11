# Running AnyTime Chat

Verified against Qt 6.11.0 / mingw 13.1 on Windows, 2026-09-07.

---

## What works today

Sign-in and Nearby, end to end, against the real C++ core:

- Account creation generates a **real X25519 + Ed25519 keypair**, derives the
  `PeerId` from its SHA-256 fingerprint, and builds the `@vinay·731a` handle
- Peers discover each other by exchanging `ANNOUNCE` packets carrying genuine
  Ed25519 signatures, verified before a peer is ever shown
- Discovery results push to the UI over SSE — no polling
- The local API is gated by a per-launch 256-bit secret path prefix

**Messaging works end to end**: chat request -> accept (which pins the peer's
authenticated static key) -> Noise-encrypted message -> ACK. Verified between two
instances from a cold start.

**Storage is durable.** SQLite backs every repository — account, peers, contacts,
the block list, requests, chats, messages and the outbox. Message bodies and
queued outbox payloads are encrypted at rest, the database runs in WAL mode, and
a message queued while a peer is away survives the app being killed.

**Images send over Bluetooth.** Built and compiling end to end, but **not yet
tried on two handsets** — treat it as unverified until you have.

The UI downscales a photo to 1600px and re-encodes it as JPEG, stepping quality
down until it fits under 192 KB (`kMaxMediaBytes`), because a phone camera
produces 3-8 MB originals. The cap is set by BLE's send queue rather than by the
protocol — see the constant for why raising it means fixing the send burst first.

**Photos travel over Nearby Connections where it is available**, which is the
whole point of that transport: BLE moves roughly 1-2 KB/s, so a capped photo
takes several minutes there, while Nearby upgrades to Wi-Fi Direct. Where Play
Services is absent, Nearby is not registered at all and photos fall back to BLE —
slow, but working. Text, discovery and handshakes never leave BLE.

See [TRANSPORTS.md](TRANSPORTS.md) for how the two transports divide the work.

Image bytes live in their own `media` table, encrypted with the same cipher as
message bodies; the message row holds only a reference, so opening a chat does
not load megabytes it will not display. Video is deliberately **not** supported:
it needs a streaming design rather than the whole-buffer encrypt path, and is
low priority for v1.

**The BLE radio works on real hardware.** Two Android handsets discover each
other, connect, exchange a chat request, accept it and message over Bluetooth
with no internet. What is still unmeasured is behaviour under *load* — several
concurrent GATT links, over hours, across chipsets. See the Android section.

> **⚠ The service UUID changed.** It was a patterned placeholder and is now a
> random v4 value, along with the RX/TX characteristic UUIDs. Devices advertise
> and filter on it, so **an old build and a new build are invisible to each
> other**. Reinstall on *every* test handset at the same time, or they will
> silently fail to see each other and look like a radio regression.

---

## Tests

```bash
cmake --build build/desktop --target anytime_tests
build/desktop/anytime_tests.exe
```

**406 assertions, all passing**, across the packet codec, fragmenter, dedup cache,
identity crypto, the Noise XX handshake, the SQLite repositories and the services
behind the screens. The codec and
fragmenter suites are mostly about what gets *rejected*, since both parse bytes
from untrusted peers.

`SqliteStoreTests.cpp` runs against a real database file in a temporary
directory. It asserts the things whose absence would only show up as lost user
data: that rows survive a close and reopen, that the block list works for a peer
who is not a contact, that history paging stays correct when two messages share a
millisecond, and that a message body genuinely does not appear as plaintext in
the file on disk — with a control assertion proving that scan can find an
unencrypted column, so it cannot pass vacuously.

`ServiceTests.cpp` drives a real `AppCore` — real SQLite, identity keys, Noise and
CBOR — with a fake clock and one capturing transport, so it can read what the
device actually broadcasts. It covers blocking (the list, the Requests tab, the
chat composer, and undoing all of it), resending a failed message and refusing to
resend anything else, and account edits reaching the radio immediately.

It also puts **two complete phones on an in-memory "air"** — real cores, real
Noise handshakes, BLE-sized 480-byte frames — and runs request → accept → chat →
photo between them. That is where padding is proven where it matters (a one-letter
reply and a paragraph are the same size to a listener), where an accept that fails
half way is shown to roll back cleanly, and where a notification is shown to fire
for the recipient, named by the pinned contact, and never for the sender.

The suite has already earned its place three times: it caught `kFixedHeaderBytes` being
declared as 12 when the fixed header is really 16 bytes (so `encodedSize()`
under-reported every frame by 4), and it proves announce signatures cannot be
forged — which the previous placeholder identity store silently failed. The
third: renaming yourself or switching off "Discoverable" in Profile was saved but
never reached the mesh, so the phone kept announcing the old name, and kept
advertising, until the next restart.

### UI tests

```bash
cd ui && npm test
```

Vitest + Testing Library in jsdom, with `api` and `onEvent` mocked — the core has
its own suite, so these assert what the user sees and which call each tap makes:
retrying a failed message (both the never-stored and the core-gave-up cases),
blocking and unblocking from a chat, declining with block, and every Profile
control, including that no placeholder profile is shown while loading.

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
npm run build:native

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

## Logs

Logging is spdlog (MIT, vendored in `third_party/spdlog`). Every line goes to the
console **and** a rotating file (5 MB × 3).

### What gets logged, per build

The level is fixed **at compile time** by `SPDLOG_ACTIVE_LEVEL`. Calls below it are
not skipped at runtime — they are not in the binary at all, so debug logging costs
a shipped build nothing.

| Build | Compiled level | Lines you get |
|---|---|---|
| `Debug` | debug | debug, info, warn, error, critical |
| `Release` / `RelWithDebInfo` / `MinSizeRel` | `ANYTIME_RELEASE_LOG_LEVEL` | default **critical** only |

To keep warnings and errors in a production build instead:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DANYTIME_RELEASE_LOG_LEVEL=warn
```

Accepted values: `trace debug info warn error critical off`.

### Where to find them

| Platform | Console | File |
|---|---|---|
| Desktop | the terminal running `anytime.exe` | `%APPDATA%/AnyTime/AnyTime Chat/[instance-<name>/]logs/anytime.log` |
| Android | `adb logcat` | `files/logs/anytime.log` in app storage |

The desktop banner prints the log path next to the URL.

On Android the C++ core logs under the tag `AnyTime`; the Java radio layer has
its own tags:

```bash
adb logcat -s AnyTime AnyTimeBle AnyTimeMesh AnyTimePermissions
adb shell run-as org.techyskitter.anytimechat.app cat files/logs/anytime.log
```

The Java tags follow the same split by build: `debug` lines only in a debuggable
APK, warnings and errors always.

Each line is tagged by area: `[app] [core] [mesh] [session] [discovery]
[transport] [loopback] [ble] [request] [chat] [delivery] [account] [identity]
[http] [sse] [qt]` — so `grep "\[session\]"` isolates handshakes.

### What is never logged

Message bodies, greetings, decrypted payloads, key material and the API token.
Peer ids, message ids, byte counts and error codes are fine. This is a rule, not
a style preference: a debug log can be pulled off a phone with one adb command.
The rules are written at the top of `core/log/Log.h`.

---

## UI build targets

| Command | Output | Used by |
|---|---|---|
| `npm run build:native` | `ui/dist/` | Desktop app, and the APK via `copy_ui_assets` |
| `npm run build:android` | `ui/dist/` | Alias of `build:native` |
| `npm run build:web` | `ui/dist-web/` | Future hosted web app |

Android and iOS share the native bundle, so iOS will get a `build:ios` alias
rather than a third build. The native and web outputs differ in `base`: native
uses relative asset paths so they survive the secret URL prefix; web uses
absolute paths so reloading a deep link such as `/chats/abc` still works.

`build:web` compiles today, but there is no web backend for it to talk to yet —
it will show "AnyTime could not start" if served.

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

Packaging is complete and **an APK has been built and verified** — 20 MB,
`org.techyskitter.anytimechat.app`, containing `lib/arm64-v8a/libanytime_arm64-v8a.so`,
`assets/www/index.html` and the React bundle, with all ten permissions present.

`platform/android/` holds the manifest, the Gradle build, the theme and the
foreground service.

### What happens on first launch

1. After sign-up the core starts the BLE transport. On first run that fails on
   purpose, because the Bluetooth permission dialog is shown **once**.
2. `AppCore` retries every 3 s (`kTransportRetryMillis`). The moment you tap
   *Allow*, the radio starts. Advertising and any scan the Nearby screen already
   asked for resume automatically, with no restart needed. The same retry covers
   Bluetooth being switched off and back on.
3. With the radio up, `MeshForegroundService` starts and shows the "AnyTime is
   on" notification. It keeps the process, and so the mesh, alive in the
   background. Declining notifications does **not** block the radio.
3a. While the app is in the background, a message or chat request posts a
   notification (`MessageNotifier.java`, fed by `adapters/notify/Notifier.cpp`).
   It names the sender by the contact name you accepted, hides the text on the
   lock screen, and a tap opens that chat or the Requests tab. Nothing is posted
   while the app is on screen, and opening the app clears them. Declining the
   notification permission simply means none are shown.
4. Links are opened by the core, not the radio. When two phones see each other,
   the one with the lower peer id connects. The other connects itself only if
   nothing arrives within 10 s. Watch it with
   `adb logcat -s AnyTime | grep "\[discovery\]"`.

### Build

```bash
cd ui && npm run build:android && cd ..  # the APK embeds ui/dist

NDK=C:/Users/<you>/AppData/Local/Android/Sdk/ndk/27.2.12479018
SDK=C:/Users/<you>/AppData/Local/Android/Sdk

cmake -S . -B build/android -G Ninja \
  -DCMAKE_MAKE_PROGRAM="C:/Qt/Tools/Ninja/ninja.exe" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  -DANDROID_STL=c++_shared \
  -DQT_HOST_PATH="C:/Qt/6.11.0/mingw_64" \
  -DCMAKE_FIND_ROOT_PATH="C:/Qt/6.11.0/android_arm64_v8a" \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.0/android_arm64_v8a" \
  -DANDROID_SDK_ROOT="$SDK" \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build/android --target apk
```

The APK lands at `build/android/android-build/anytime.apk`. Install it with:

```bash
adb install -r build/android/android-build/anytime.apk
```

Or just open `CMakeLists.txt` in Qt Creator and select the
**Android Qt 6.11.0 Clang arm64-v8a** kit.

### The assets pipeline

`copy_ui_assets` stages `ui/dist/` into `platform/android/assets/www` on every
Android build, and `anytime` depends on it. The local HTTP server reads from
`assets:/www` at runtime.

`platform/android/assets/` is gitignored: it is generated, and a stale committed
copy is exactly the kind of thing nobody notices until the app ships with last
week's screens. **If you change the UI, re-run `npm run build:android` before packaging.**
CMake warns at configure time when `ui/dist/index.html` is missing.

### The BLE radio

`BleGattTransport` + `BleRadio.java` are implemented and **verified between two
Android handsets** — discovery, linking, requests and messaging all work over the
radio. What follows is how it works; the remaining unknown is endurance, not
whether it functions.

**Both roles at once.** Every device advertises *and* scans, so a link is either
outgoing (we are central: we write their RX characteristic, subscribe to their TX)
or incoming (we are peripheral: they write our RX, we notify on our TX). The send
path differs per role, which is why `Link` records which one it is.

**Where the peer id lives.** A 128-bit service UUID nearly fills the 31-byte
advertisement, so the 8-byte peer id goes in the **scan response** as manufacturer
data. Android merges both into one `ScanResult`.

**Chunking, and why it is not a dumb-pipe violation.** Default ATT MTU is 23 bytes
— 20 usable — against a 180-byte protocol frame. `BleRadio` therefore splits each
frame across MTU-sized chunks behind a 2-byte length prefix and reassembles before
handing it up. That is link framing, exactly like the length prefix
`LoopbackTransport` uses over TCP. The `ITransport` contract is *complete frames in,
complete frames out*; surviving a 20-byte MTU is the transport's own problem. The
radio still never inspects a byte it carries. An MTU of 517 is requested to keep
the chunk count down.

**The thread hop.** Every Android BLE callback arrives on a binder thread. Each
JNI entry point marshals its arguments and posts them with
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; nothing touches core state
from a binder thread. The `jbyteArray` is copied at the boundary because the local
reference dies before the queued call runs.

**Permissions are requested before the GATT server opens.** Without
`BLUETOOTH_SCAN` on API 31+, scanning does not fail — it silently returns nothing,
which is indistinguishable from "nobody is nearby" and is the most confusing way
for this to break. `PermissionBridge` handles the API 31 split.

**Scanning is duty-cycled.** `DiscoveryMode` now reaches the radio: `Active` maps
to `SCAN_MODE_LOW_LATENCY` (continuous) and `Passive` to `SCAN_MODE_LOW_POWER`
(roughly a 10% duty cycle). `NearbyScreen` asks for active while it is on screen
and drops to passive when it unmounts — it never *stops*, or inbound requests
would stop arriving too. `POST /api/discovery` takes `{"mode":"active"|"passive"}`
and defaults to active.

**Battery-optimisation exemption is deliberately not prompted for.** Google Play
prohibits requesting a Doze exemption unless the app's core function is adversely
affected without one, and names foreground services as the preferred alternative —
which `MeshForegroundService` already is, with the `connectedDevice` type that
exempts it from most background restrictions. `PermissionBridge` exposes
`isIgnoringBatteryOptimizations()` and `openBatteryOptimizationSettings()` so a
settings row can let the user opt in themselves; do not wire either to start-up.

### What the APK does today

Installs, launches, loads the React UI in the platform WebView through the
secret-prefix URL, completes account creation, starts the BLE radio, **finds
other handsets, and holds a conversation with them over Bluetooth.**

**What is still untested is endurance.** Concurrent advertise-plus-scan with
several GATT links over hours is the least-exercised path on real hardware and
varies by chipset. Two phones working for a few minutes does not predict six
phones working for an hour, which is what the phase 2 spike measures:

> Two or three handsets, each advertising, scanning, connecting to everything it
> finds, and sending a **sequence-numbered** counter on every open link every five
> seconds for an hour. The sequence numbers are the point — they make loss
> measurable rather than merely suspected. Watch packet loss per link, whether it
> grows over time, whether advertising survives, and memory. Run it across Exynos,
> Snapdragon and MediaTek.

### Permissions

The manifest declares two Bluetooth permission sets because Qt 6.11 targets
minSdk 28 and `BLUETOOTH_SCAN` / `ADVERTISE` / `CONNECT` only exist from API 31:

| API | Permissions |
|---|---|
| 31+ | `BLUETOOTH_SCAN` (with `neverForLocation`), `BLUETOOTH_ADVERTISE`, `BLUETOOTH_CONNECT` |
| 28–30 | `BLUETOOTH`, `BLUETOOTH_ADMIN`, `ACCESS_FINE_LOCATION` (all `maxSdkVersion="30"`) |

`neverForLocation` is why the app avoids a location prompt on modern Android. On
28–30 there is no such flag — scanning simply required location back then, and
the only way to avoid it entirely would be raising minSdk to 31.

`INTERNET` is also declared, which looks wrong in an offline app. Android gates
`socket()` behind it even for `127.0.0.1`, and the UI reaches the core over a
loopback HTTP server. The server binds `LocalHost` only and refuses non-loopback
peers.

`allowBackup` is **false**: the identity key file is not yet Keystore-wrapped, and
letting it into a cloud backup would put the device's private key somewhere we do
not control.

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

## Placeholders

One component is still a deliberate placeholder, down from three. It is loud
about it in its own header, and it is replaced without touching anything above it.

### Identity crypto is now REAL

`MonocypherIdentityStore` replaced the fake one. Real Ed25519 signatures, real
X25519 keys, `PeerId` derived from a genuine public key. `tests/IdentityTests.cpp`
proves forgery is rejected — another identity's signature, a signature derived
from the public key, a one-bit tamper, a modified message, and the wrong
verification key are all refused.

**Still owed:** secret keys sit unencrypted on disk. Android Keystore wrapping is
the remaining half of the at-rest story and changes only this adapter.

### Persistence is now REAL

`SqliteStore` replaced `SettingsStore`, which kept everything but the account in
RAM. Schema v1 covers account, peer, contact, blocked, request, chat, message and
outbox, in WAL mode with forward-only migrations stamped in `user_version`.

`body_encrypted` and `payload_encrypted` hold XChaCha20-Poly1305 ciphertext from
`MonocypherDataCipher`. The 24-byte nonce variant is deliberate: random nonces
are only safe at that width, so there is no counter to persist and therefore no
way to reuse one across a crash.

`SettingsStore` is still compiled, for one reason only — `main.cpp` uses it once
at startup to import an account written by an older build into SQLite, so
updating the app does not look like being signed out. The INI is left in place
rather than deleted, so a downgrade still works.

**Still owed:** the data key sits in a file next to the database with owner-only
permissions, exactly like the identity key. Keystore wrapping covers both at once.

### Session encryption is now REAL

`Noise_XX_25519_ChaChaPoly_BLAKE2b`, in `adapters/crypto/`. Every primitive comes
from Monocypher — X25519, RFC 8439 ChaCha20-Poly1305, BLAKE2b — and what is
hand-written is the protocol state machine the Noise spec defines: HMAC, HKDF,
CipherState, SymmetricState, HandshakeState.

`tests/NoiseTests.cpp` asserts the properties whose absence would not otherwise
be visible: both sides derive the same keys in the right direction, each learns
the other's *real* static key, a tampered handshake aborts rather than
downgrading, a forged frame does not desynchronise the counter, and two
handshakes between the same identities derive different keys.

**Still owed:** validation against the published Noise test vectors. A round-trip
between two of our own implementations proves they agree with each other, not
that they agree with the specification.
