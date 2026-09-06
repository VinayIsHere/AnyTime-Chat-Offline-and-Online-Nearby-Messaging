# Skeleton guide

What exists in the tree today, what is deliberately empty, and what to build next.

Read [ARCHITECTURE.md](ARCHITECTURE.md) first — this file only describes the
*state of the scaffolding*.

---

## What is here

Every interface and class declaration for phases 0 and 1, with the reasoning
attached to the code that implements it. Headers are complete; `.cpp` files are
not yet written.

| Area | Files | State |
|---|---|---|
| `core/domain/` | `Types.h`, `Result.h`, `Entities.h` | Complete |
| `core/ports/` | 6 interface headers | **Complete — this is the architecture** |
| `core/protocol/` | `Packet.h`, `PacketCodec.h`, `DedupCache.h`, `Fragmenter.h`, `AppEnvelope.h` | Declared |
| `core/services/` | 9 service headers | Declared |
| `core/AppCore.h` | Composition façade + `IMeshListener` | Declared |
| `adapters/` | http, persistence, transport ×2, identity, crypto, codec, clock | Declared |
| `app/` | `main.cpp`, two QML shells | **`main.cpp` is complete** — read it for the wiring |
| `platform/android/` | `BleRadio.java` | Signature only, throws `UnsupportedOperationException` |
| `scripts/` | `check-core-includes.sh` | **Working** — run it |

`app/main.cpp` is the single file that knows which concrete adapter backs each
port. Read it to understand the wiring.

---

## The rules the code encodes

These are load-bearing. Each is stated in the header of the file that depends on
it, so they are hard to violate accidentally.

1. **`core/` never includes Qt, SQLite, JNI or platform headers.**
   Enforced by `scripts/check-core-includes.sh`. Wire it into CI before writing
   the first `.cpp`.

2. **The native radio is a dumb pipe.** `BleRadio.java` and its iOS counterpart
   advertise, scan, link and move bytes. No framing, no TTL, no fragmentation,
   no Noise. See `adapters/transport/ble/BleGattTransport.h`.

3. **Noise sessions key on `PeerId`, never `LinkId`.** A session opened over BLE
   must still encrypt bulk sent over Nearby Connections in phase 5. See
   `core/services/SessionManager.h`.

4. **Fragmentation lives in `core/protocol`, not the radio.** MTU belongs to the
   transport, which makes it tempting to split down there — that means writing it
   again in Swift. See `core/protocol/Fragmenter.h`.

5. **Every JNI callback hops to the core thread before touching core state.**
   The core is single-threaded by design. See `BleGattTransport.h`.

6. **`QHttpServer` stays out of the build.** GPLv3-or-commercial; it would
   relicense the app. See the licence gate in `CMakeLists.txt`.

---

## The UI

Moved out of `claude_design/src_v1/` into `ui/`, and it **typechecks and builds**
(`npm run build` → `dist/index.html` + one 282 kB bundle). Run it today with
`cd ui && npm run dev`.

Three changes were needed on the way in:

- **Username availability check removed** (AD-05). With no server there is no
  registry to check against. The field now explains the fingerprint
  discriminator instead of pretending to reserve a name.
- **`RequestsScreen` added.** `BottomNav` had always linked `/requests`; nothing
  was there.
- **`NearbyScreen` polls no more.** The 2.5 s loop is replaced by
  `api/events.ts`, an SSE subscription to `peer.discovered` / `peer.lost`.

### The secret path prefix, end to end

`ui/index.html` carries one placeholder the C++ server substitutes while serving:

```html
<script>window.__AT_BASE__ = '%%AT_BASE%%';</script>
```

**Substitution contract:** replace `%%AT_BASE%%` with `/<64-hex-token>/`. It
occurs exactly once in the built document, so replace-first and replace-all are
equivalent. The placeholder and the global are spelled *differently* on purpose —
if both were `__AT_BASE__`, a replace-all would rewrite the property name into
`window./abc123/` and break the page.

`ui/src/api/base.ts` reads it, falls back to `/` when unreplaced (that is
`vite dev`), and exports `BASE_PATH`, `ROUTER_BASENAME` and `apiUrl()`. Nothing
else in the UI thinks about tokens.

`vite.config.ts` sets `base: './'` so asset URLs stay relative — an absolute
`/assets/index.js` would resolve outside the token prefix and 404.

### Running the UI before the core exists

`ui/src/api/devMock.ts` fakes the account endpoints in `localStorage` so the
sign-in flow works in a browser today. It is guarded three ways (`import.meta.env.DEV`,
`IS_NATIVE_SHELL`, and a dynamic import) so it cannot reach a shipped build.

It is **not** a protocol test double — that role belongs to the loopback
transport (AD-07), which exercises the real C++ core.

## Deliberately not here yet

- **All `.cpp` implementations.** Headers first, on purpose — the interfaces are
  the thing worth reviewing before any of them is filled in.
- **`tests/`** — `CMakeLists.txt` references it behind `ANYTIME_BUILD_TESTS`.
  Needs GoogleTest wired up plus `FakeClock`, `FakeTransport`, `InMemoryStore`.
- **`platform/android/AndroidManifest.xml` and `build.gradle`** — permissions are
  listed in ARCHITECTURE.md §14.
- **libsodium / noise-c fetch logic** in CMake.
- **A real service UUID.** `core/protocol/Packet.h` carries a placeholder;
  generate a random one before the first field build.

---

## Build order — phase 0

The dependency chain, shortest path to two desktop windows talking:

```
1.  core/domain/*.cpp                  trivial; unblocks everything
2.  scripts/check-core-includes.sh     into CI now, while core/ is still clean
3.  tests/ harness + FakeClock         so 4 and 5 are testable as written
4.  core/protocol/PacketCodec.cpp      the most test-heavy file in the project
5.  core/protocol/DedupCache.cpp
6.  adapters/clock/QtClock.cpp
7.  adapters/http/*                    router, SSE hub, static assets
8.  adapters/transport/loopback/       ← the phase 0 payoff (AD-07)
9.  core/services/*                    wire the flow end to end
10. ui/                                move in, swap polling for SSE
```

Steps 1–7 have no radio and no crypto. Step 8 is where two desktop instances
first see each other.

**Start at 8 conceptually, build 1–7 to get there.** The loopback transport is
what makes every later phase cheap, and it validates the ports boundary while the
boundary is still soft enough to move.

---

## Build state

The project **configures and builds today**, verified against Qt 6.11 / NDK 27
clang for `arm64-v8a`.

```
[1/5] Building CXX object .../core/domain/Result.cpp.o
[2/5] Building CXX object .../core/domain/Entities.cpp.o
[3/5] Building CXX object .../core/domain/Types.cpp.o
[4/5] Building CXX object .../core/HeaderCheck.cpp.o
[5/5] Linking CXX static library libanytime_core.a
```

Two CMake options control how much is built:

| Option | Default | Meaning |
|---|---|---|
| `ANYTIME_BUILD_APP` | `OFF` | Adapters + the Qt shell. Turn on once `adapters/*.cpp` exist — until then the app target would fail to link. |
| `ANYTIME_BUILD_TESTS` | `ON` | No-op until `tests/CMakeLists.txt` exists (guarded by an `EXISTS` check). |

`core/HeaderCheck.cpp` includes **every** header under `core/`, so the compiler
validates all of them on every build even before their `.cpp` files exist. It
also carries static assertions pinning the wire-format constants — packet header
size, peer id width, TTL, the retry ladder length. Add a line to it whenever you
add a header.

The phase 0 work queue lives as a commented block in `CMakeLists.txt`. Uncomment
each line as you write the file.

## Verifying the skeleton

```bash
# The architecture rule — passes on a clean tree
scripts/check-core-includes.sh
```
