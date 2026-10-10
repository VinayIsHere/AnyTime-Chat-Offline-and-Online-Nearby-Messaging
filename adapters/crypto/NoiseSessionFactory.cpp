// adapters/crypto/NoiseSessionFactory.cpp

#include "adapters/crypto/NoiseSessionFactory.h"

#include <cstring>

#include "adapters/crypto/NoiseProtocol.h"
#include "core/ports/IIdentityStore.h"

namespace anytime::crypto {

namespace {

using namespace anytime::crypto::noise;

class NoiseSession final : public ISecureSession {
public:
    NoiseSession(IIdentityStore& identity, const PublicKey& staticPublicKey)
        : identity_(identity), staticPublicKey_(staticPublicKey)
    {
    }

    Result beginHandshake(HandshakeRole role, ByteBuffer& out) override
    {
        if (started_) {
            return Result::error(ErrorCode::HandshakeFailed, "Handshake already started.");
        }

        KeyBytes staticPub{};
        std::memcpy(staticPub.data(), staticPublicKey_.data(), kKeyLen);

        // The static secret stays inside the identity store; Noise reaches it
        // only through this callback.
        auto staticDh = [this](const DhBytes& theirPublic, DhBytes& sharedOut) -> bool {
            PublicKey peer{};
            std::memcpy(peer.data(), theirPublic.data(), kKeyLen);

            auto shared = identity_.agree(peer);
            if (shared.isError()) return false;

            std::memcpy(sharedOut.data(), shared.value().data(), kKeyLen);
            return true;
        };

        const Role noiseRole =
            role == HandshakeRole::Initiator ? Role::Initiator : Role::Responder;

        if (const Result r = handshake_.initialize(noiseRole, staticPub, staticDh);
            r.isError()) {
            return r;
        }

        started_ = true;
        role_ = role;

        // The initiator opens with message 1; the responder waits.
        if (role == HandshakeRole::Initiator) return handshake_.writeMessage(ByteSpan{}, out);

        out.clear();
        return Result::ok();
    }

    Result readHandshake(ByteSpan message, ByteBuffer& out) override
    {
        out.clear();
        if (!started_) {
            return Result::error(ErrorCode::HandshakeFailed, "Handshake not started.");
        }
        if (handshake_.isComplete()) {
            return Result::error(ErrorCode::HandshakeFailed, "Handshake already complete.");
        }

        ByteBuffer payload;
        if (const Result r = handshake_.readMessage(message, payload); r.isError()) return r;

        // A reply is due unless that read completed the handshake.
        if (!handshake_.isComplete()) return handshake_.writeMessage(ByteSpan{}, out);
        return Result::ok();
    }

    bool isEstablished() const override { return handshake_.isComplete(); }

    Outcome<PublicKey> remoteStaticKey() const override
    {
        KeyBytes key{};
        if (!handshake_.remoteStaticKey(key)) {
            return Outcome<PublicKey>::error(ErrorCode::PreconditionFailed,
                                             "Handshake has not authenticated a peer yet.");
        }
        PublicKey out{};
        std::memcpy(out.data(), key.data(), kKeyLen);
        return out;
    }

    Result encrypt(ByteSpan plaintext, ByteBuffer& out) override
    {
        if (!handshake_.isComplete()) {
            return Result::error(ErrorCode::PreconditionFailed, "Session is not established.");
        }
        // Transport messages carry no associated data.
        return handshake_.sendingCipher().encryptWithAd(ByteSpan{}, plaintext, out);
    }

    Result decrypt(ByteSpan ciphertext, ByteBuffer& out) override
    {
        if (!handshake_.isComplete()) {
            return Result::error(ErrorCode::PreconditionFailed, "Session is not established.");
        }
        return handshake_.receivingCipher().decryptWithAd(ByteSpan{}, ciphertext, out);
    }

private:
    IIdentityStore& identity_;
    PublicKey       staticPublicKey_;
    HandshakeState  handshake_;
    HandshakeRole   role_ = HandshakeRole::Initiator;
    bool            started_ = false;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

NoiseSessionFactory::NoiseSessionFactory(IIdentityStore& identity) : identity_(identity) {}

NoiseSessionFactory::~NoiseSessionFactory() = default;

Outcome<std::unique_ptr<ISecureSession>> NoiseSessionFactory::createSession(const PublicKey*)
{
    auto identity = identity_.loadOrCreate();
    if (identity.isError()) return identity.result();

    return std::unique_ptr<ISecureSession>(
        std::make_unique<NoiseSession>(identity_, identity.value().staticPublicKey));
}

bool NoiseSessionFactory::isHandshakeInitiation(ByteSpan message) const
{
    return message.size() == noise::kKeyLen;
}

}  // namespace anytime::crypto
