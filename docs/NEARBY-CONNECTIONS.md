# Nearby Connections — proposed architecture

> **⚠ SUPERSEDED — kept as the design record.**
> This was the proposal. Nearby Connections is now **implemented**, and the living
> document describing both transports is **[TRANSPORTS.md](TRANSPORTS.md)** — read that
> first. Two things changed on the way from this proposal to the code: Nearby also
> performs discovery (in `Active` mode only), and it is registered conditionally on Play
> Services being present. Everything below is preserved because the *reasoning* — the
> costs, the endpoint-id problem, the "Nearby never discovers" debate — is still the
> argument that shaped the result.

**Status:** implemented; see TRANSPORTS.md. Phase 5 (docs/ARCHITECTURE.md §15).
**Goal:** photos between two people in the same room, at Wi-Fi speed.

Read [ARCHITECTURE.md](ARCHITECTURE.md) §06 (transport abstraction) and §11 (the
radio seam) first. This document only describes what a second transport adds.

---

## 01 · Why this transport, and what it is not

BLE carries about 1–2 KB/s in practice. A 3 MB photo is roughly **forty minutes**.
That is not a tuning problem — it is the wrong radio. Nearby Connections
discovers over Bluetooth and then upgrades the actual transfer to Wi-Fi Direct,
reaching tens of MB/s.

**It does not replace BLE.** BLE stays the mesh and control transport: it is
cheap, always-on, and works without user consent. Nearby is expensive, needs a
consent dialog, and is Android-only. It is a *bulk pipe opened on demand*, not a
second way to be discovered.

> **The whole point of the seam.** `NearbyConnectionsTransport` is a new class
> implementing `ITransport`. `ChatService`, `RequestService`, `MeshEngine` and
> `SessionManager` do not change. If this proposal requires editing any of them,
> the design is wrong and the seam was not worth building.

---

## 02 · Three things that make it nearly free

These already exist and were built for exactly this moment:

1. **`PeerId` is transport-independent** (AD-05). The same person seen over BLE
   and over Wi-Fi is one identity, because identity is a key fingerprint rather
   than an address.
2. **`PeerLinks` already holds several links per peer**, keyed by `TransportId`
   ([TransportRegistry.h](../core/services/TransportRegistry.h)). Nothing needs
   widening to hold a second link to the same person.
3. **The Noise session binds to the peer, not the link**
   ([SessionManager.h](../core/services/SessionManager.h)). You handshake once
   over BLE, and that same cipher state encrypts a video sent over Wi-Fi ten
   minutes later. **No second handshake and no second trust decision** — which
   also means no second place for a stranger to be accepted.

`TransportCapabilities` even carries `requiresUserConsent` already, which exists
solely because Nearby and Multipeer show system UI.

---

## 03 · Shape of the change

```
adapters/transport/nearby/
  NearbyConnectionsTransport.h/.cpp     ITransport + JNI binding
platform/android/src/com/anytimechat/nearby/
  NearbyRadio.java                      the dumb pipe, same rules as BleRadio
```

`NearbyRadio.java` keeps the **same dumb-pipe contract** as `BleRadio.java`:
bytes in, bytes out, no protocol logic, every callback posted to the core thread
before it touches core state. If its method list starts diverging from
`BleRadio`'s shape, logic has leaked downward.

One line in `main.cpp` registers it:

```cpp
#ifdef Q_OS_ANDROID
    core.addTransport(std::make_unique<transport::BleGattTransport>(bleConfig));
    core.addTransport(std::make_unique<transport::NearbyConnectionsTransport>(nearbyConfig));
#endif
```

`RoutingPolicy` then picks it for `PayloadClass::Bulk` because it advertises
`supportsBulk`. That is the entire integration.

### Capabilities it reports

| Field | Value | Why |
|---|---|---|
| `maxFramePayload` | 32 KB | Nearby's `Payload.Type.BYTES` ceiling; it does its own reliable chunking, so the core `Fragmenter` effectively no-ops here |
| `throughputClass` | 2 (High) | Wi-Fi Direct |
| `supportsBulk` | **true** | the flag `RoutingPolicy` selects on |
| `supportsMesh` | **false** | point-to-point only — it must never be chosen to relay |
| `requiresUserConsent` | **true** | Nearby shows a system dialog |
| `energyCost` | High | which is why links are opened on demand and closed when idle |

---

## 04 · The two genuinely hard problems

### a. Nearby gives you an `endpointId`, not a `PeerId`

Nearby identifies peers with an opaque, per-session string. Our whole model is
keyed on the 8-byte key fingerprint. Bridging them:

- **Advertise the local `PeerId` hex as the Nearby endpoint name.** The name is
  attacker-controlled, so it is a *routing hint only*.
- Maintain `endpointId ↔ PeerId` in the Java layer, exactly as `BleRadio` already
  maps `deviceAddress ↔ PeerId`.
- **Identity is still proven by the Noise session, never by the endpoint name.**
  A link comes up unbound and carries no directed traffic until an authenticated
  ANNOUNCE binds it — the mechanism `TransportRegistry::bindLink()` already
  implements for incoming BLE links. Reuse it verbatim; do not add a second
  trust path.

### b. Discovery would be duplicated

Nearby has its own advertise/discover cycle. Running it continuously alongside
BLE means two discovery systems, double the battery, and two answers to "who is
nearby" that can disagree.

**Proposal: Nearby never discovers.** BLE remains the only discovery mechanism.
`startDiscovery()` on this transport is a **no-op that returns Ok**. Nearby
advertising and discovery are started *only* for the seconds around an actual
bulk transfer, targeted at one peer, then stopped.

So the sequence for sending a photo is:

```
1. BLE already has a session with this peer (PeerId known, Noise established)
2. User attaches a photo               → PayloadClass::Bulk
3. RoutingPolicy: no Nearby link open  → ask the transport to open one
4. NearbyRadio: brief advertise+discover, find the endpoint whose name is that PeerId
5. requestConnection → consent dialog on both sides → connected
6. Encrypt with the EXISTING BLE-derived Noise session, send
7. Idle timeout (~30 s) → disconnect, stop Wi-Fi Direct
```

Step 6 is the payoff. The user accepted this person once, over BLE, and is not
asked to trust them again.

---

## 05 · The costs, stated plainly

These are real and should be decided on before any code is written.

**Google Play Services becomes a hard dependency.** `play-services-nearby` is
the first external runtime dependency in the project. It adds APK size, and the
app stops working on de-Googled devices (GrapheneOS, LineageOS without GApps,
Huawei). For an offline-first, no-server, privacy-shaped app, that audience
overlaps uncomfortably with the users most likely to want it. **BLE messaging
must keep working when Play Services is absent** — this transport has to
degrade to "not registered", never to a crash.

**The location permission comes back.** Nearby needs `NEARBY_WIFI_DEVICES` on
API 33+, but `ACCESS_FINE_LOCATION` below that. §14 went to real trouble to avoid
the location prompt with `neverForLocation` on `BLUETOOTH_SCAN`, precisely
because it causes install abandonment. Adopting Nearby reintroduces it for every
device under API 33 — unless the permission is requested lazily, at the moment
the user first sends a photo, rather than at start-up. **Request it lazily.**

**A consent dialog per connection.** Unavoidable; it is the API's design. It is
tolerable once per transfer session, not per photo.

**Wi-Fi Direct is disruptive.** It can drop the device's normal Wi-Fi association
on some chipsets. Another reason for short, on-demand links rather than a
standing one.

---

## 06 · What this does NOT cover

Sending a photo is more than a fast pipe. Also needed, all *above* the transport:

- A new `EnvelopeType` (`chat.media`) and its CBOR shape — note the forward
  compatibility rule in [AppEnvelope.h](../core/protocol/AppEnvelope.h): older
  builds must skip it, not crash.
- A `blob` table plus on-disk storage for the media itself, encrypted at rest
  with the same `IDataCipher` the message bodies use. Message rows should hold a
  reference, never the bytes.
- Thumbnails, which have to travel over **BLE** so a chat list renders before any
  Wi-Fi link exists.
- Progress and cancellation in the UI, and a delivery state machine richer than
  the current `queued → sent → delivered`.
- A size cap and a decision about retention (open question §16.2).

**Recommended order:** do the app-layer media work first, over BLE only, with a
small size cap. That proves the envelope, storage and UI against a transport
that already works. Add Nearby afterwards as a pure speed optimisation — at
which point it is genuinely a one-class change, which is the claim the seam was
built to make true.

---

## 07 · iOS

`MultipeerConnectivity` is the same Bluetooth-discovers / Wi-Fi-carries pattern
and would slot in as a third `ITransport` with the same design. **Explicitly
deprioritised: version 1 is Android-only.** Nothing in this proposal should be
shaped around iOS beyond keeping the transport seam honest.
