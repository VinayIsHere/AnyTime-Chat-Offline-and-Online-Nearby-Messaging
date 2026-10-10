// core/services/MeshEngine.cpp
//
// The protocol loop: announces, the Noise handshake, encrypted envelopes, acks
// and fragments. Read the header for the inbound and outbound pipelines.

#include "core/services/MeshEngine.h"

#include <cstring>
#include <random>

#include "core/log/Log.h"
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

    AT_LOG_INFO("mesh", "started as {} ({})", localPeerId_.toHex(),
                discoverable_ ? "discoverable" : "hidden");

    scheduleAnnounce();
    return Result::ok();
}

void MeshEngine::stop()
{
    if (running_) AT_LOG_INFO("mesh", "stopped");
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
        announceTimer_ = kInvalidTimer;
        emitAnnounce();
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
    if (const Result r = protocol::PacketCodec::encode(toEncode, unsigned_); r.isError()) {
        AT_LOG_ERROR("mesh", "could not encode announce: {}", r.message());
        return;
    }

    auto signature = identity_.sign(unsigned_);
    if (signature.isError()) {
        AT_LOG_ERROR("mesh", "could not sign announce: {}", signature.result().message());
        return;
    }

    toEncode.signature = ByteSpan{signature.value().data(), signature.value().size()};

    ByteBuffer frame;
    if (const Result r = protocol::PacketCodec::encode(toEncode, frame); r.isError()) {
        AT_LOG_ERROR("mesh", "could not encode signed announce: {}", r.message());
        return;
    }

    AT_LOG_TRACE("mesh", "announce ({} bytes)", frame.size());
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

Result MeshEngine::sendDirected(const PeerId& peerId, protocol::PacketType type,
                                ByteSpan payload, PayloadClass cls)
{
    protocol::PacketToEncode toEncode;
    toEncode.type = type;
    toEncode.ttl = protocol::kInitialTtl;
    toEncode.timestamp = static_cast<std::uint32_t>(clock_.now() / 1000);
    toEncode.sender = localPeerId_;
    toEncode.recipient = peerId;
    toEncode.payload = payload;
    toEncode.pad = (type == protocol::PacketType::NoiseMessage);

    ByteBuffer frame;
    if (const Result r = protocol::PacketCodec::encode(toEncode, frame); r.isError()) {
        AT_LOG_ERROR("mesh", "could not encode packet type {} for {}: {}",
                     static_cast<int>(type), peerId.toHex(), r.message());
        return r;
    }

    // RANKED, AND TRIED IN ORDER — not one answer taken on trust.
    //
    // The best transport by CAPABILITY frequently has no open link to this peer.
    // That is the normal state for Nearby, whose links are opened on demand for
    // a transfer and torn down afterwards. Taking select()'s single answer meant
    // a photo picked Nearby, found no link, failed, backed off, and repeated
    // that through all eight attempts before being marked Failed a day later —
    // while a healthy BLE link sat open the entire time.
    //
    // So: walk the ranked list, skip anything with no link, and fall back. This
    // fixes every payload class at once, not just Bulk.
    const std::vector<TransportId> ranked = routing_.candidates(peerId, cls);
    if (ranked.empty()) {
        AT_LOG_DEBUG("mesh", "no transport reaches {}", peerId.toHex());
        return Result::error(ErrorCode::TransportUnavailable, "No transport can reach that peer.");
    }

    const PeerLinks* links = registry_.linksFor(peerId);
    Result lastError = Result::error(ErrorCode::LinkClosed, "No open link to that peer.");

    for (const TransportId candidate : ranked) {
        ITransport* transport = registry_.find(candidate);
        if (transport == nullptr) continue;

        const LinkId link = links ? links->linkFor(candidate) : kInvalidLink;
        if (link == kInvalidLink) continue;  // ranked highly, but cannot carry anything

        const Result sent = sendFrame(*transport, link, frame, peerId, toEncode.timestamp);
        if (sent.isOk()) {
            // Worth a log line: a photo quietly travelling over BLE instead of
            // Wi-Fi is the difference between seconds and minutes, and it is
            // otherwise invisible.
            if (candidate != ranked.front()) {
                AT_LOG_INFO("mesh", "{} unavailable for {}; fell back to {}",
                            toString(ranked.front()), peerId.toHex(), toString(candidate));
            }
            return Result::ok();
        }
        lastError = sent;
    }

    return lastError;
}

Result MeshEngine::sendFrame(ITransport& transport, LinkId link, const ByteBuffer& frame,
                             const PeerId& recipient, std::uint32_t timestamp)
{
    const std::size_t budget = transport.caps().maxFramePayload;
    if (frame.size() <= budget) return transport.send(link, frame);

    // THE BUDGET MUST PAY FOR THE WRAPPER TOO.
    //
    // Every slice below is re-wrapped in its own directed Fragment packet, and
    // that header was not being accounted for: splitting to 480 and then adding
    // 28 bytes produced 508-byte frames — larger than the budget they were
    // fragmented to. That breaks ITransport::send's stated contract that the
    // core guarantees the fit, and LoopbackTransport rejects such a frame
    // outright, so fragmentation could never have worked over loopback at all.
    //
    // BLE never complained only because its radio re-chunks whatever it is
    // given. At MTU 517 the 508-byte frame plus BleRadio's 2-byte length prefix
    // came to 510 of a 512-byte write: two bytes short of costing every fragment
    // a SECOND chunk, which for a photo means ~520 chunks against
    // MAX_QUEUED_CHUNKS = 512 and a link that starts refusing frames.
    //
    // Derived rather than written as 28, because widening kPayloadLengthBytes is
    // exactly the kind of change that would otherwise spend that margin unseen.
    const std::size_t wrapperBytes = protocol::kFixedHeaderBytes
                                     + protocol::kRecipientFieldBytes
                                     + protocol::kPayloadLengthBytes;
    if (budget <= wrapperBytes + protocol::kFragmentHeaderBytes) {
        AT_LOG_ERROR("mesh", "transport frame budget of {} cannot carry a fragment", budget);
        return Result::error(ErrorCode::InvalidArgument,
                             "Transport frame budget is too small to carry a fragment.");
    }

    auto fragments = protocol::Fragmenter::split(
        frame, budget - wrapperBytes,
        protocol::Fragmenter::newFragmentId(localPeerId_, clock_.now()));
    if (fragments.isError()) {
        AT_LOG_ERROR("mesh", "could not fragment a {}-byte frame: {}", frame.size(),
                     fragments.result().message());
        return fragments.result();
    }

    AT_LOG_TRACE("mesh", "{}-byte frame to {} split into {} fragments", frame.size(),
                 recipient.toHex(), fragments.value().size());

    for (const ByteBuffer& slice : fragments.value()) {
        protocol::PacketToEncode wrapper;
        wrapper.type = protocol::PacketType::Fragment;
        wrapper.ttl = protocol::kInitialTtl;
        wrapper.timestamp = timestamp;
        wrapper.sender = localPeerId_;
        wrapper.recipient = recipient;
        wrapper.payload = slice;

        ByteBuffer wrapped;
        if (const Result r = protocol::PacketCodec::encode(wrapper, wrapped); r.isError()) {
            return r;
        }
        // A mid-frame failure leaves the far end holding fragments it can never
        // complete; the reassembly timeout discards them. Reporting the error
        // lets the caller try the next transport with a fresh fragment id.
        if (const Result r = transport.send(link, wrapped); r.isError()) return r;
    }
    return Result::ok();
}

Result MeshEngine::ensureSession(const PeerId& peerId)
{
    // A handshake that lost a packet would otherwise sit "in flight" forever,
    // and every envelope queued behind it with it.
    sessions_.expireIfStale(peerId);

    const SessionState state = sessions_.stateFor(peerId);
    if (state == SessionState::Established || state == SessionState::Handshaking) {
        return Result::ok();
    }

    ByteBuffer message1;
    if (const Result r = sessions_.beginHandshake(peerId, message1); r.isError()) return r;
    if (message1.empty()) return Result::ok();

    // Control: a handshake must go over a mesh transport, never a bulk one that
    // needs consent before it can carry anything.
    const Result sent =
        sendDirected(peerId, protocol::PacketType::NoiseHandshake, message1, PayloadClass::Control);
    if (sent.isError()) {
        // Nothing reached the peer, so there is no handshake to wait for. Drop it
        // now rather than after the timeout, so the next attempt starts at once.
        sessions_.forget(peerId);
    }
    return sent;
}

Result MeshEngine::sendEnvelope(const PeerId& peerId, const protocol::AppEnvelope& envelope,
                                PayloadClass cls)
{
    if (sessions_.stateFor(peerId) != SessionState::Established) {
        // Hold it and start the handshake. flushPending() sends it the moment
        // the session lands — without this the first envelope to any new peer is
        // lost, and the first envelope is always the chat request.
        if (const Result r = ensureSession(peerId); r.isError()) return r;

        auto& queue = pendingEnvelopes_[peerId];
        if (queue.size() < kMaxPendingPerPeer) {
            queue.push_back(PendingEnvelope{envelope, cls});
            AT_LOG_DEBUG("mesh", "envelope for {} held until the session is up ({} waiting)",
                         peerId.toHex(), queue.size());
        } else {
            AT_LOG_WARN("mesh", "pending queue for {} is full; envelope left to the outbox",
                        peerId.toHex());
        }

        return Result::error(ErrorCode::PreconditionFailed, "Session is still handshaking.");
    }

    ByteBuffer plaintext;
    if (const Result r = cbor_.encode(envelope, plaintext); r.isError()) {
        AT_LOG_ERROR("mesh", "could not encode envelope type {}: {}",
                     static_cast<int>(envelope.type), r.message());
        return r;
    }

    // Padded BEFORE encryption so the filler is ciphertext too — see
    // kPaddingBuckets. sendDirected sets the flag that tells the far side.
    const ByteBuffer padded = protocol::padPlaintext(plaintext);

    ByteBuffer ciphertext;
    if (const Result r = sessions_.encrypt(peerId, padded, ciphertext); r.isError()) {
        AT_LOG_WARN("mesh", "could not encrypt for {}: {}", peerId.toHex(), r.message());
        return r;
    }

    return sendDirected(peerId, protocol::PacketType::NoiseMessage, ciphertext, cls);
}

Result MeshEngine::sendAck(const PeerId& peerId, const ClientMessageId& clientMessageId)
{
    // Acks ride outside the Noise session on purpose: they must still flow when a
    // session has been torn down, and they reveal only that something arrived,
    // which anyone counting packets already knows.
    const std::string& id = clientMessageId.value();
    ByteBuffer payload(id.begin(), id.end());
    return sendDirected(peerId, protocol::PacketType::Ack, payload, PayloadClass::Control);
}

// ── ITransportListener ───────────────────────────────────────────────────────

void MeshEngine::onPeerSeen(TransportId id, const PeerHandle& handle)
{
    lastRssi_[handle.peerId] = handle.rssi;
    if (peerSeenHook_) peerSeenHook_(id, handle);
}

void MeshEngine::onLinkUp(TransportId, LinkId, const PeerId&)
{
    // Announce straight away rather than at the next timer tick, which can be up
    // to 30 s off. An incoming link cannot carry directed traffic until the far
    // side hears who we are — and neither can theirs, until we hear them.
    emitAnnounce();
}

void MeshEngine::onLinkDown(TransportId, LinkId, LinkCloseReason) {}

void MeshEngine::onTransportStateChanged(TransportId, bool available)
{
    if (radioStateHook_) radioStateHook_(available);
}

void MeshEngine::onFrame(TransportId id, LinkId link, ByteSpan frame)
{
    protocol::Packet packet;
    if (const Result r = protocol::PacketCodec::decode(frame, packet); r.isError()) {
        AT_LOG_DEBUG("mesh", "undecodable {}-byte frame on {} link {}: {}", frame.size(),
                     toString(id), link, r.message());
        return;
    }

    // Our own broadcast coming back to us over another link.
    if (packet.sender == localPeerId_) return;

    // Controlled flooding without this loops packets between neighbours until
    // TTL burns out, multiplying traffic and battery cost.
    if (!dedup_.observe(packet, clock_.now())) return;

    handlePacket(id, link, packet, lastRssi_.count(packet.sender) ? lastRssi_[packet.sender] : 0);
}

void MeshEngine::handlePacket(TransportId transport, LinkId link, const protocol::Packet& packet,
                              int rssi)
{
    switch (packet.type) {
        case protocol::PacketType::Announce:
            handleAnnounce(transport, link, packet, rssi);
            break;

        case protocol::PacketType::NoiseHandshake:
            if (addressedToUs(packet)) handleHandshake(packet);
            break;

        case protocol::PacketType::NoiseMessage:
            if (addressedToUs(packet)) handleNoiseMessage(packet);
            break;

        case protocol::PacketType::Ack:
            if (addressedToUs(packet)) handleAck(packet);
            break;

        case protocol::PacketType::Fragment:
            if (addressedToUs(packet)) handleFragment(transport, link, packet, rssi);
            break;
    }
}

void MeshEngine::handleAnnounce(TransportId transport, LinkId link,
                                const protocol::Packet& packet, int rssi)
{
    std::string nickname;
    PublicKey staticKey{};
    PublicKey signingKey{};

    if (!decodeAnnounce(packet.payload, nickname, staticKey, signingKey)) {
        AT_LOG_DEBUG("mesh", "malformed announce from {}", packet.sender.toHex());
        return;
    }

    // An unsigned announce is not evidence of anything — anyone can claim any
    // nickname. Verification is what makes the 60 s reachability window mean
    // "this peer is really here" (section 11).
    if (!packet.isSigned() || packet.signature.size() != kSignatureBytes) {
        AT_LOG_DEBUG("mesh", "unsigned announce from {} ignored", packet.sender.toHex());
        return;
    }

    Signature signature{};
    std::memcpy(signature.data(), packet.signature.data(), kSignatureBytes);

    ByteBuffer reencoded;
    protocol::PacketToEncode toEncode;
    toEncode.type = packet.type;
    toEncode.ttl = packet.ttl;
    toEncode.timestamp = packet.timestamp;
    toEncode.sender = packet.sender;
    toEncode.payload = packet.payload;
    if (protocol::PacketCodec::encode(toEncode, reencoded).isError()) return;

    if (!identity_.verify(reencoded, signature, signingKey)) {
        AT_LOG_WARN("mesh", "announce claiming {} failed signature verification",
                    packet.sender.toHex());
        return;
    }

    // A valid signature proves only that the sender holds the signing key IN the
    // packet. The peer id must also derive from the static key in it, or anyone
    // could sign an announce with their own keys under someone else's id.
    if (identity_.peerIdFor(staticKey) != packet.sender) {
        AT_LOG_WARN("mesh", "announce claiming {} carries a key for a different peer",
                    packet.sender.toHex());
        return;
    }

    // Un-relayed (full TTL) means the sender is on the other end of THIS link, so
    // an incoming link that arrived anonymous can now carry directed traffic.
    if (packet.ttl == protocol::kInitialTtl && link != kInvalidLink) {
        registry_.bindLink(transport, link, packet.sender);
    }

    ++knownPeerCount_;
    if (listener_) listener_->onAnnounce(packet.sender, nickname, staticKey, rssi);
}

void MeshEngine::handleHandshake(const protocol::Packet& packet)
{
    const PeerId& peer = packet.sender;

    ByteBuffer reply;
    if (const Result r = sessions_.readHandshake(peer, packet.payload, reply); r.isError()) {
        // Drop rather than retry. A peer that cannot complete XX is either
        // hostile or running a different build; the next send starts afresh.
        sessions_.forget(peer);
        return;
    }

    const bool established = sessions_.stateFor(peer) == SessionState::Established;

    // Noise proves who we talked to; this checks that it is who the packet CLAIMS
    // to be from. Checked before our final reply goes out, so a mismatched peer
    // never completes the handshake with us.
    if (established) {
        const auto remote = sessions_.remoteStaticKey(peer);
        if (remote.isError() || identity_.peerIdFor(remote.value()) != peer) {
            AT_LOG_WARN("mesh", "handshake claiming {} authenticated a different key; dropped",
                        peer.toHex());
            sessions_.forget(peer);
            pendingEnvelopes_.erase(peer);
            return;
        }
    }

    if (!reply.empty()) {
        if (const Result r = sendDirected(peer, protocol::PacketType::NoiseHandshake, reply,
                                          PayloadClass::Control);
            r.isError()) {
            AT_LOG_DEBUG("mesh", "handshake reply to {} not sent: {}", peer.toHex(), r.message());
        }
    }

    if (established) {
        flushPending(peer);
        if (listener_) listener_->onSessionEstablished(peer);
    }
}

void MeshEngine::handleNoiseMessage(const protocol::Packet& packet)
{
    // Every v3 sender pads. An unpadded message is a peer leaking lengths, or
    // one whose flag was altered in flight; either way it is not accepted, so
    // the privacy property cannot quietly regress on one side only.
    if (!packet.isPadded()) {
        AT_LOG_DEBUG("mesh", "unpadded message from {} dropped", packet.sender.toHex());
        return;
    }

    ByteBuffer padded;
    if (const Result r = sessions_.decrypt(packet.sender, packet.payload, padded);
        r.isError()) {
        // Forged, replayed, or from a session we no longer hold (for instance
        // after this app restarted). The sender's outbox retries after re-keying.
        AT_LOG_DEBUG("mesh", "could not decrypt message from {}: {}", packet.sender.toHex(),
                     r.message());
        return;
    }

    // The length prefix is trustworthy only because the AEAD just authenticated
    // it; unpadPlaintext still bounds-checks it rather than assume.
    ByteBuffer plaintext;
    if (const Result r = protocol::unpadPlaintext(padded, plaintext); r.isError()) {
        AT_LOG_DEBUG("mesh", "badly padded message from {}: {}", packet.sender.toHex(),
                     r.message());
        return;
    }

    protocol::AppEnvelope envelope;
    if (const Result r = cbor_.decode(plaintext, envelope); r.isError()) {
        // An envelope type a newer build introduced — skip it, do not crash.
        AT_LOG_DEBUG("mesh", "undecodable envelope from {}: {}", packet.sender.toHex(),
                     r.message());
        return;
    }

    if (listener_) listener_->onEnvelope(packet.sender, envelope);
}

void MeshEngine::handleAck(const protocol::Packet& packet)
{
    const std::string id(reinterpret_cast<const char*>(packet.payload.data()),
                         packet.payload.size());
    if (id.empty()) return;
    if (listener_) listener_->onAck(packet.sender, ClientMessageId{id});
}

void MeshEngine::flushPending(const PeerId& peerId)
{
    const auto found = pendingEnvelopes_.find(peerId);
    if (found == pendingEnvelopes_.end()) return;

    // Move first: sendEnvelope re-enters this class, and iterating a container
    // it might touch is asking for trouble.
    const std::vector<PendingEnvelope> queued = std::move(found->second);
    pendingEnvelopes_.erase(found);

    AT_LOG_DEBUG("mesh", "sending {} envelope(s) held for {}", queued.size(), peerId.toHex());

    for (const PendingEnvelope& pending : queued) {
        sendEnvelope(peerId, pending.envelope, pending.cls);
    }
}

bool MeshEngine::addressedToUs(const protocol::Packet& packet) const
{
    // Directed traffic for someone else is a relay candidate, never ours to
    // process. Without this a third device in range would try, and fail, to
    // decrypt every private message it could hear.
    return packet.isBroadcast() || packet.recipient == localPeerId_;
}

void MeshEngine::handleFragment(TransportId transport, LinkId link,
                                const protocol::Packet& packet, int rssi)
{
    auto assembled = reassembler_.accept(packet.sender, packet.payload, clock_.now());
    if (assembled.isError()) {
        AT_LOG_DEBUG("mesh", "fragment from {} rejected: {}", packet.sender.toHex(),
                     assembled.result().message());
        return;
    }
    if (!assembled.value().has_value()) return;  // more pieces to come

    protocol::Packet inner;
    if (protocol::PacketCodec::decode(*assembled.value(), inner).isError()) {
        AT_LOG_DEBUG("mesh", "reassembled frame from {} does not decode", packet.sender.toHex());
        return;
    }
    if (inner.type == protocol::PacketType::Fragment) return;  // no nesting

    // Where the pieces actually arrived — not a hard-coded transport. The inner
    // packet may be an announce, and binding a link needs the real one.
    handlePacket(transport, link, inner, rssi);
}

void MeshEngine::maybeRelay(const protocol::Packet&, TransportId)
{
    // NOT IMPLEMENTED, AND NOT CALLED. Read this before assuming otherwise.
    //
    // AD-09 is deferred: v1 is direct-range only. What exists today is the
    // GROUNDWORK, not the behaviour —
    //
    //   present:  the TTL field in the header, set to kInitialTtl on every
    //             outgoing packet; the LRU dedup cache, already consulted in
    //             onFrame(); addressedToUs(), which identifies traffic meant for
    //             someone else.
    //   absent:   decrementing TTL, clamping it on dense graphs, re-broadcasting
    //             to every OTHER link, and a call site — handlePacket() simply
    //             drops packets that are not addressed to us.
    //
    // So setting relayEnabled_ = true changes nothing at all. Enabling relay is
    // a real feature, not a flag: see docs/ARCHITECTURE.md AD-09 for the cost
    // (every device forwards strangers' traffic — roughly double the battery and
    // a much larger test surface) and why direct-only was chosen for v1.
    if (!relayEnabled_) return;
}

}  // namespace anytime
