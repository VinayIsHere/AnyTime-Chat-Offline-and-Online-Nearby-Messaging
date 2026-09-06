// core/services/MeshEngine.cpp
//
// This milestone implements the ANNOUNCE path end to end — which is all that
// Nearby needs. The Noise handshake, encrypted envelopes and acks are declared
// and routed but return NotImplemented until the messaging milestone; they are
// wired here so adding them changes only the bodies below.

#include "core/services/MeshEngine.h"

#include <cstring>
#include <random>

#include "core/ports/IClock.h"
#include "core/ports/IIdentityStore.h"
#include "core/protocol/PacketCodec.h"
#include "core/services/RoutingPolicy.h"
#include "core/services/SessionManager.h"
#include "core/services/TransportRegistry.h"

namespace anytime {

namespace {

/// ANNOUNCE payload:
///   [ nicknameLen : u8 ][ nickname ][ staticPublicKey : 32 ][ signingPublicKey : 32 ]
///
/// Cleartext by design — a peer must be able to read it before any session
/// exists. That is why announces are signed rather than encrypted, and why
/// docs/ARCHITECTURE.md section 10 states plainly that nicknames and public keys
/// are observable to a passive listener.
constexpr std::size_t kAnnounceFixedBytes = 1 + kPublicKeyBytes * 2;

ByteBuffer encodeAnnounce(const std::string& nickname, const PublicKey& staticKey,
                          const PublicKey& signingKey)
{
    const std::size_t nameLength = std::min<std::size_t>(nickname.size(), 255);

    ByteBuffer out;
    out.reserve(kAnnounceFixedBytes + nameLength);
    out.push_back(static_cast<Byte>(nameLength));
    out.insert(out.end(), nickname.begin(), nickname.begin() + nameLength);
    out.insert(out.end(), staticKey.begin(), staticKey.end());
    out.insert(out.end(), signingKey.begin(), signingKey.end());
    return out;
}

bool decodeAnnounce(ByteSpan payload, std::string& nickname, PublicKey& staticKey,
                    PublicKey& signingKey)
{
    if (payload.empty()) return false;

    const std::size_t nameLength = payload[0];
    if (payload.size() != 1 + nameLength + kPublicKeyBytes * 2) return false;

    nickname.assign(reinterpret_cast<const char*>(payload.data()) + 1, nameLength);
    std::memcpy(staticKey.data(), payload.data() + 1 + nameLength, kPublicKeyBytes);
    std::memcpy(signingKey.data(), payload.data() + 1 + nameLength + kPublicKeyBytes,
                kPublicKeyBytes);
    return true;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

MeshEngine::MeshEngine(TransportRegistry& registry, SessionManager& sessions,
                       RoutingPolicy& routing, IIdentityStore& identity,
                       protocol::ICborCodec& cbor, IClock& clock)
    : registry_(registry),
      sessions_(sessions),
      routing_(routing),
      identity_(identity),
      cbor_(cbor),
      clock_(clock)
{
}

MeshEngine::~MeshEngine() { stop(); }

void MeshEngine::setListener(IMeshListener* listener) { listener_ = listener; }

Result MeshEngine::start(const Account& account)
{
    localPeerId_ = account.peerId;
    nickname_ = account.username;
    discoverable_ = account.discoverable;
    staticKey_ = account.staticPublicKey;
    signingKey_ = account.signingPublicKey;
    running_ = true;

    scheduleAnnounce();
    return Result::ok();
}

void MeshEngine::stop()
{
    running_ = false;
    if (announceTimer_ != kInvalidTimer) {
        clock_.cancel(announceTimer_);
        announceTimer_ = kInvalidTimer;
    }
}

void MeshEngine::updateAnnounceContent(const std::string& nickname, bool discoverable)
{
    nickname_ = nickname;
    discoverable_ = discoverable;
}

void MeshEngine::scheduleAnnounce()
{
    if (!running_) return;
    if (announceTimer_ != kInvalidTimer) clock_.cancel(announceTimer_);

    // Fast while isolated so discovery feels instant, backing off once peers are
    // known. The jitter matters: without it, devices started together stay in
    // lockstep and collide on every broadcast.
    Timestamp delay = protocol::kAnnounceIsolatedMillis;
    if (!registry_.available().empty() && knownPeerCount_ > 0) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<Timestamp> jitter(protocol::kAnnounceConnectedMinMillis,
                                                        protocol::kAnnounceConnectedMaxMillis);
        delay = jitter(rng);
    }

    announceTimer_ = clock_.scheduleOnce(delay, [this] {
        emitAnnounce();
        announceTimer_ = kInvalidTimer;
        scheduleAnnounce();
    });
}

void MeshEngine::emitAnnounce()
{
    if (!running_ || !discoverable_) return;

    const ByteBuffer payload = encodeAnnounce(nickname_, staticKey_, signingKey_);

    protocol::PacketToEncode toEncode;
    toEncode.type = protocol::PacketType::Announce;
    toEncode.ttl = protocol::kInitialTtl;
    toEncode.timestamp = static_cast<std::uint32_t>(clock_.now() / 1000);
    toEncode.sender = localPeerId_;
    toEncode.payload = payload;  // broadcast: recipient left null

    // Sign the frame as it will appear on the wire, minus the signature itself.
    ByteBuffer unsigned_;
    if (protocol::PacketCodec::encode(toEncode, unsigned_).isError()) return;

    auto signature = identity_.sign(unsigned_);
    if (signature.isError()) return;

    toEncode.signature = ByteSpan{signature.value().data(), signature.value().size()};

    ByteBuffer frame;
    if (protocol::PacketCodec::encode(toEncode, frame).isError()) return;

    broadcastFrame(frame);
}

void MeshEngine::broadcastFrame(const ByteBuffer& frame)
{
    // Announces go to every mesh-capable transport, on every open link, plus the
    // transport's own broadcast channel where it has one.
    for (ITransport* transport : registry_.available()) {
        if (!transport->caps().supportsMesh) continue;
        transport->send(kInvalidLink, frame);  // kInvalidLink == "broadcast"
    }
}

Result MeshEngine::ensureSession(const PeerId&)
{
    return Result::error(ErrorCode::Internal,
                         "Secure sessions arrive with the messaging milestone.");
}

Result MeshEngine::sendEnvelope(const PeerId&, const protocol::AppEnvelope&, PayloadClass)
{
    return Result::error(ErrorCode::Internal,
                         "Encrypted messaging arrives with the messaging milestone.");
}

Result MeshEngine::sendAck(const PeerId&, const ClientMessageId&)
{
    return Result::error(ErrorCode::Internal,
                         "Encrypted messaging arrives with the messaging milestone.");
}

// ── ITransportListener ───────────────────────────────────────────────────────

void MeshEngine::onPeerSeen(TransportId id, const PeerHandle& handle)
{
    lastRssi_[handle.peerId] = handle.rssi;
    if (peerSeenHook_) peerSeenHook_(id, handle);
}

void MeshEngine::onLinkUp(TransportId, LinkId, const PeerId&) {}

void MeshEngine::onLinkDown(TransportId, LinkId, LinkCloseReason) {}

void MeshEngine::onTransportStateChanged(TransportId, bool available)
{
    if (radioStateHook_) radioStateHook_(available);
}

void MeshEngine::onFrame(TransportId id, LinkId link, ByteSpan frame)
{
    protocol::Packet packet;
    if (protocol::PacketCodec::decode(frame, packet).isError()) return;

    // Our own broadcast coming back to us over another link.
    if (packet.sender == localPeerId_) return;

    // Controlled flooding without this loops packets between neighbours until
    // TTL burns out, multiplying traffic and battery cost.
    if (!dedup_.observe(packet, clock_.now())) return;

    handlePacket(id, link, packet, lastRssi_.count(packet.sender) ? lastRssi_[packet.sender] : 0);
}

void MeshEngine::handlePacket(TransportId, LinkId, const protocol::Packet& packet, int rssi)
{
    switch (packet.type) {
        case protocol::PacketType::Announce:
            handleAnnounce(packet, rssi);
            break;

        // Declared, routed, and deliberately inert until the messaging milestone.
        case protocol::PacketType::NoiseHandshake:
        case protocol::PacketType::NoiseMessage:
        case protocol::PacketType::Fragment:
        case protocol::PacketType::Ack:
            break;
    }
}

void MeshEngine::handleAnnounce(const protocol::Packet& packet, int rssi)
{
    std::string nickname;
    PublicKey staticKey{};
    PublicKey signingKey{};

    if (!decodeAnnounce(packet.payload, nickname, staticKey, signingKey)) return;

    // An unsigned announce is not evidence of anything — anyone can claim any
    // nickname. Verification is what makes the 60 s reachability window mean
    // "this peer is really here" (section 11).
    if (!packet.isSigned()) return;

    Signature signature{};
    if (packet.signature.size() != kSignatureBytes) return;
    std::memcpy(signature.data(), packet.signature.data(), kSignatureBytes);

    ByteBuffer reencoded;
    protocol::PacketToEncode toEncode;
    toEncode.type = packet.type;
    toEncode.ttl = packet.ttl;
    toEncode.timestamp = packet.timestamp;
    toEncode.sender = packet.sender;
    toEncode.payload = packet.payload;
    if (protocol::PacketCodec::encode(toEncode, reencoded).isError()) return;

    if (!identity_.verify(reencoded, signature, signingKey)) return;

    ++knownPeerCount_;
    if (listener_) listener_->onAnnounce(packet.sender, nickname, staticKey, rssi);
}

void MeshEngine::handleHandshake(const protocol::Packet&) {}
void MeshEngine::handleNoiseMessage(const protocol::Packet&) {}
void MeshEngine::handleAck(const protocol::Packet&) {}

void MeshEngine::maybeRelay(const protocol::Packet&, TransportId)
{
    // AD-09 is still open. The TTL field and the dedup cache both exist, so
    // turning relay on is a policy flip here rather than a format change.
    if (!relayEnabled_) return;
}

}  // namespace anytime
