# Transports — BLE and Nearby Connections

How AnyTime moves bytes between two phones, which radio carries what, and why.

Read [ARCHITECTURE.md](ARCHITECTURE.md) §06 (transport abstraction) and §11 (the radio
seam) first. This document is the concrete detail behind them, covering **both** shipped
Android transports in one place.

> Supersedes the proposal in [NEARBY-CONNECTIONS.md](NEARBY-CONNECTIONS.md), which is kept
> as the original design record.

---

## 01 · The seam

Every radio implements one interface, `core/ports/ITransport.h`:

```
id()  caps()  isAvailable()
start(localPeerId)  stop()
startDiscovery(DiscoveryMode)  stopDiscovery()
startAdvertising()  stopAdvertising()
openLink(PeerHandle) -> LinkId   closeLink(LinkId)   send(LinkId, frame)
setListener(ITransportListener)
```

and reports exactly four things upward: `onPeerSeen`, `onLinkUp`, `onLinkDown`, `onFrame`
(plus `onTransportStateChanged`).

**Everything above this line is written once.** Framing, TTL, the dedup cache,
fragmentation, Noise, announce scheduling, the outbox — none of it lives in a transport. A
transport is a *dumb pipe*: bytes in, bytes out, and it never inspects a byte it carries.

Three implementations exist today:

| Transport | Where | Role |
|---|---|---|
| `BleGattTransport` | `adapters/transport/ble/` + `BleRadio.java` | **Primary.** Discovery, presence, handshakes, text |
| `NearbyConnectionsTransport` | `adapters/transport/nearby/` + `NearbyRadio.java` | **Secondary.** Photos, and extended-range discovery while the Nearby screen is open |
| `LoopbackTransport` | `adapters/transport/loopback/` | Desktop development — there is deliberately no desktop radio (AD-07) |

---

## 02 · Why BLE is primary and Nearby is secondary

This was a real decision, not a default. Three reasons, in order of weight:

**a. Nearby allows one `Strategy` per app.** Discovering many peers wants `P2P_CLUSTER`;
bulk transfer wants `P2P_POINT_TO_POINT`, the fastest of the three. You cannot have both.
Making Nearby the general-purpose transport would force `P2P_CLUSTER` permanently and cap
photo transfer at the **slowest** strategy — throwing away the only reason to adopt it. So
Nearby runs `P2P_POINT_TO_POINT` and carries bulk only.

**b. Play Services.** Nearby cannot run without it. If it were primary, a de-Googled phone
(GrapheneOS, LineageOS without GApps, Huawei) would discover **nobody** — and that audience
overlaps heavily with people who want an offline mesh messenger. As secondary, such a phone
merely sends photos slowly over BLE.

**c. Battery.** Nearby discovery drives BLE, classic Bluetooth *and* Wi-Fi scanning
simultaneously. Our own BLE scan is duty-cycled (`SCAN_MODE_LOW_POWER` in the background).
Running Nearby discovery continuously would be brutal.

**Net effect:** the app's core function — finding people, establishing trust, exchanging
messages — never depends on Google. Only photo *speed* does.

---

## 03 · Capabilities, and how routing uses them

Each transport advertises `TransportCapabilities`; `RoutingPolicy` scores them per
`PayloadClass`. This is the entire selection mechanism.

| | BLE | Nearby | Loopback |
|---|---|---|---|
| `maxFramePayload` | 480 B | 32 KB | 480 B |
| `throughputClass` | 0 (Low) | 2 (High) | 0 |
| `supportsBulk` | ✗ | **✓** | ✗ |
| `supportsMesh` | **✓** | ✗ | ✓ |
| `requiresUserConsent` | ✗ | **✓** | ✗ |
| `energyCost` | Low | High | Low |

**The rules**, from `RoutingPolicy::candidates()`:

- **Control** (handshakes, acks) — *must* be `supportsMesh`. Nearby is excluded outright: it
  is point-to-point and cannot reach a stranger without a consent dialog.
- **Text** — cheapest energy wins, and `requiresUserConsent` costs 500 points. Asking
  someone to approve a connection to send one line would be absurd.
- **Bulk** — `supportsBulk` is worth 1000, then throughput. Nearby wins when present; **BLE
  is the documented fallback** when it is not, so a photo always sends, just slowly.

### Fallback is real, and it has to be

`RoutingPolicy` ranks on **capability alone** — it deliberately does not know which
transports have an open link. So the top-ranked answer is frequently unable to carry
anything: Nearby links are opened on demand for a transfer and torn down afterwards, so
"available but no open link" is its *normal* resting state.

`MeshEngine::sendDirected` therefore walks the ranked list, **skips any transport with no
open link, and falls back to the next**. It also re-fragments per attempt, because the
budget is per transport: a frame split for Nearby's 32 KB cannot be handed to BLE's 480.

> Without this the failure was ugly and silent: a photo picked Nearby, found no link,
> failed, backed off, and repeated that through all eight retry attempts before being
> marked **Failed a day later** — while a perfectly healthy BLE link sat open the whole
> time. Falling back to a slow transport beats failing on a fast one.

A fallback is logged at info level, because a photo quietly taking minutes over BLE instead
of seconds over Wi-Fi is otherwise invisible.

All of this is pinned by `tests/RoutingPolicyTests.cpp` with fake transports, so the rules
are provable on a desktop with no radio.

---

## 04 · How a payload reaches the right radio

`PayloadClass` is decided by whoever composes the message and travels all the way down:

```
ChatService::send      -> PayloadClass::Text
ChatService::sendMedia -> PayloadClass::Bulk
        |
DeliveryService::enqueue  stores it ON the outbox entry  (schema v3: outbox.payload_class)
        |
DeliveryService::attemptSend  -> mesh_.sendEnvelope(peer, envelope, entry.payloadClass)
        |
MeshEngine::sendDirected(..., cls)  -> RoutingPolicy::select(peer, cls)
        |
TransportRegistry::find(chosen)  ->  ITransport::send(link, frame)
```

> **This chain was broken until recently, and silently.** `DeliveryService` hard-coded
> `PayloadClass::Text` and `MeshEngine::sendEnvelope` discarded its third argument
> entirely — so `Bulk` never once reached `RoutingPolicy`. Nothing failed; the right answer
> was simply never asked for. A bulk transport could have been registered and photos would
> still have gone over BLE.
>
> **The class is persisted, not re-derived.** By the time a packet exists, every app payload
> is a `NoiseMessage` and the distinction is gone — so inferring it at send time is
> impossible. It is stored on the outbox row precisely so a photo queued before the app was
> killed is still `Bulk` when the app comes back.

---

## 05 · BLE: the primary transport

### Discovery

Every device **advertises and scans at once**. The 31-byte advertisement holds the 128-bit
service UUID; the 8-byte peer id goes in the **scan response** as manufacturer data, which
Android merges into one `ScanResult`.

Scanning is duty-cycled off `DiscoveryMode`:

| Mode | Android scan mode | When |
|---|---|---|
| `Active` | `SCAN_MODE_LOW_LATENCY` | Nearby screen is open |
| `Passive` | `SCAN_MODE_LOW_POWER` (~10% duty) | Background — keeps inbound requests arriving |
| `Off` | scan stopped | Bluetooth off |

The UI asks for `active` on mount and drops to `passive` on unmount — never *off*, or
inbound requests would stop arriving.

### Liveness comes from announces, never link events

A peer is reachable because a **signature-verified `ANNOUNCE`** arrived inside the 60 s
window; three missed announces mark it lost. BLE connection callbacks are unreliable on
every platform, so link up/down is a resource concern, not a correctness one.

### Links, roles and chunking

A link is *outgoing* (we are central: we write their RX, subscribe to their TX) or
*incoming* (we are peripheral: they write our RX, we notify on our TX). `Link` records
which, because the send path differs.

Default ATT MTU is 23 bytes — 20 usable — against a 480-byte protocol frame. `BleRadio`
therefore requests MTU **517** and chunks each frame behind a 2-byte length prefix.

> `usableChunk()` clamps to **512**, because the Bluetooth spec caps an attribute value at
> 512 octets while `mtu - 3` on a 517 MTU computes 514. Writing 514 gets silently truncated
> by the peer stack, desynchronising the frame stream rather than failing loudly.

At MTU 517 a whole 480-byte frame plus its prefix lands in **one write**.

**Writes are queued per link.** Android GATT runs one operation at a time per connection; a
second write issued before the first completes is refused, so chunks are sent only from the
previous chunk's completion callback.

---

## 06 · Nearby Connections: the bulk transport

### Lifecycle — asymmetric on purpose

| | When it runs | Why |
|---|---|---|
| **Advertising** | whenever the transport is up | otherwise a peer could receive photos from us but never *send* us one |
| **Discovery** | `DiscoveryMode::Active` only | it drives three radios at once; the background is exactly when it must not run |

`startDiscovery()` therefore stops the scan for `Passive` and `Off` alike.

### Identity

Nearby identifies peers with an opaque, per-session `endpointId`. Our model is keyed on the
8-byte key fingerprint. The bridge:

- The local **`PeerId` hex is advertised as the endpoint name**, so `onEndpointFound` can map
  `endpointId -> PeerId`.
- That name is attacker-controlled, so it is a **routing hint only**.
- `PeerHandle::address` carries the `endpointId`, exactly as it carries a MAC for BLE — the
  core never interprets either.

### Connections are auto-accepted, deliberately

`onConnectionInitiated` offers verification digits meant for two humans to compare. We
accept without showing them — **the authentication that matters is the Noise XX handshake
running inside this pipe**, which pins the peer's static key (§08). Nearby's channel is
treated as untrusted transport, exactly like BLE. Showing a second, weaker human-comparison
step would imply a guarantee we do not rely on.

### No chunking here

Unlike `BleRadio`, `NearbyRadio` does no chunking: Nearby performs its own reliable
segmentation, so a `BYTES` payload arrives whole or not at all. That is why this transport
advertises a 32 KB frame — the core fragments a photo into **tens** of frames instead of
~1,100.

### Registered conditionally — and why that matters

```cpp
if (transport::NearbyConnectionsTransport::isSupportedOnThisDevice()) {
    core.addTransport(...);
}
```

`TransportRegistry::allAvailable()` is true only when **every registered** transport reports
itself usable, and `AppCore` retries transports every `kTransportRetryMillis` until it is.
Registering a permanently-dead Nearby on a de-Googled phone would spin that retry **every
three seconds for the life of the process** — a battery drain caused purely by bookkeeping.
Skipping registration leaves one transport, and everything degrades cleanly.

---

## 07 · Threading — the rule both transports obey

The C++ core is **single-threaded by design**. Android delivers BLE callbacks on **binder
threads** and Nearby callbacks on the **main looper** — neither is the core thread.

Every JNI entry point therefore does nothing but marshal its arguments and post them with
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. The `jbyteArray` is **copied at the
boundary**, because the local reference dies before the queued call runs.

Skip this and you get intermittent corruption under concurrent links — which presents as a
radio problem rather than a threading one.

---

## 08 · Permissions, per transport

| | BLE | Nearby |
|---|---|---|
| API 31+ | `BLUETOOTH_SCAN` (`neverForLocation`), `BLUETOOTH_ADVERTISE`, `BLUETOOTH_CONNECT` | — |
| API 33+ | — | `NEARBY_WIFI_DEVICES` (`neverForLocation`), `ACCESS_WIFI_STATE`, `CHANGE_WIFI_STATE` |
| API 28–32 | `BLUETOOTH`, `BLUETOOTH_ADMIN`, `ACCESS_FINE_LOCATION` | `ACCESS_FINE_LOCATION` |
| Requested | at start-up — the radio is the product | **lazily, on the first photo** |

**Why the location ceiling moved from 30 to 32.** BLE alone avoids location from API 31 via
`neverForLocation`. Nearby does not: below API 33 it requires `ACCESS_FINE_LOCATION`
outright, with no equivalent flag. Asking at launch would put a location prompt in front of
every API 31–32 user before they had any idea why — so it is requested when they first
attach a photo, where the reason is obvious and declining costs only speed.

---

## 09 · How it degrades

| Situation | Result |
|---|---|
| Play Services absent | Nearby not registered. Discovery, chat and photos all work; photos are slow |
| Nearby permission declined | `RoutingPolicy` finds no available bulk transport; photos fall back to BLE |
| Bluetooth off | No discovery at all. Queued messages wait in the durable outbox |
| Peer out of range | Outbox retries on the backoff ladder; drains when the peer reappears |
| Both radios up | Text on BLE, photos on Nearby, handshakes always on BLE |

---

## 10 · What is tested, and what is not

**Unit tested** (`anytime_tests`, 248 assertions):

- `RoutingPolicyTests` — every selection rule above, with fake transports
- `MediaEnvelopeTests` — the image envelope, size refusal, forward compatibility
- `SqliteStoreTests` — media blobs encrypted at rest, `payload_class` surviving storage
- plus the packet codec, fragmenter, dedup cache, identity crypto and Noise handshake

**Not tested — be honest about this:**

- **Nearby has never run on hardware.** The whole path compiles and gradle builds the Java,
  but no photo has crossed a Wi-Fi Direct link.
- **BLE image transfer has never run on hardware** either, though BLE text has.
- **Endurance.** Concurrent advertise-plus-scan with several links over hours varies sharply
  by chipset. This is what the phase 2 spike exists to measure.

---

## 11 · Adding a third transport

The work is a new `ITransport` and its platform pipe. Nothing above the seam changes:

1. Implement `ITransport`; report honest `caps()` — `supportsBulk` and `supportsMesh` are
   what routing actually reads.
2. Keep the platform side a dumb pipe with the same nine-down / five-up shape.
3. Marshal every callback onto the core thread.
4. Register it in `main.cpp`, guarded if it can be unavailable.

`MultipeerConnectivity` for iOS is the obvious next one — same Bluetooth-discovers /
Wi-Fi-carries pattern. **Explicitly deprioritised: v1 is Android only.**
