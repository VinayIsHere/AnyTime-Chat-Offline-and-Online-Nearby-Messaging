// adapters/crypto/NoiseProtocol.cpp

#include "adapters/crypto/NoiseProtocol.h"

#include <QByteArray>
#include <QRandomGenerator>

#include <cstring>

extern "C" {
#include "third_party/monocypher/monocypher.h"
}

namespace anytime::crypto::noise {

namespace {

void randomBytes(Byte* out, std::size_t count)
{
    QByteArray buffer(static_cast<int>(count), Qt::Uninitialized);
    QRandomGenerator::system()->generate(
        reinterpret_cast<quint32*>(buffer.data()),
        reinterpret_cast<quint32*>(buffer.data() + buffer.size()));
    std::memcpy(out, buffer.constData(), count);
    crypto_wipe(reinterpret_cast<std::uint8_t*>(buffer.data()), count);
}

/// Noise nonce: 32 bits of zeros followed by the little-endian counter.
void nonceFor(std::uint64_t n, Byte out[12])
{
    std::memset(out, 0, 4);
    for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<Byte>((n >> (8 * i)) & 0xFF);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// HMAC-BLAKE2b  (RFC 2104 construction, block length 128)
// ─────────────────────────────────────────────────────────────────────────────

void hmac(const Byte* key, std::size_t keyLen, const Byte* data, std::size_t dataLen,
          HashBytes& out)
{
    Byte block[kBlockLen];
    std::memset(block, 0, kBlockLen);

    if (keyLen > kBlockLen) {
        HashBytes digest{};
        crypto_blake2b(digest.data(), kHashLen, key, keyLen);
        std::memcpy(block, digest.data(), kHashLen);
        crypto_wipe(digest.data(), kHashLen);
    } else {
        std::memcpy(block, key, keyLen);
    }

    Byte inner[kBlockLen];
    Byte outer[kBlockLen];
    for (std::size_t i = 0; i < kBlockLen; ++i) {
        inner[i] = static_cast<Byte>(block[i] ^ 0x36);
        outer[i] = static_cast<Byte>(block[i] ^ 0x5C);
    }

    HashBytes innerHash{};
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, kHashLen);
    crypto_blake2b_update(&ctx, inner, kBlockLen);
    crypto_blake2b_update(&ctx, data, dataLen);
    crypto_blake2b_final(&ctx, innerHash.data());

    crypto_blake2b_init(&ctx, kHashLen);
    crypto_blake2b_update(&ctx, outer, kBlockLen);
    crypto_blake2b_update(&ctx, innerHash.data(), kHashLen);
    crypto_blake2b_final(&ctx, out.data());

    crypto_wipe(block, kBlockLen);
    crypto_wipe(inner, kBlockLen);
    crypto_wipe(outer, kBlockLen);
    crypto_wipe(innerHash.data(), kHashLen);
}

void hkdf(const HashBytes& chainingKey, ByteSpan ikm, int outputs, HashBytes& out1,
          HashBytes& out2, HashBytes* out3)
{
    HashBytes tempKey{};
    hmac(chainingKey.data(), kHashLen, ikm.data(), ikm.size(), tempKey);

    const Byte one = 0x01;
    hmac(tempKey.data(), kHashLen, &one, 1, out1);

    Byte second[kHashLen + 1];
    std::memcpy(second, out1.data(), kHashLen);
    second[kHashLen] = 0x02;
    hmac(tempKey.data(), kHashLen, second, kHashLen + 1, out2);

    if (outputs == 3 && out3 != nullptr) {
        Byte third[kHashLen + 1];
        std::memcpy(third, out2.data(), kHashLen);
        third[kHashLen] = 0x03;
        hmac(tempKey.data(), kHashLen, third, kHashLen + 1, *out3);
        crypto_wipe(third, sizeof third);
    }

    crypto_wipe(tempKey.data(), kHashLen);
    crypto_wipe(second, sizeof second);
}

// ─────────────────────────────────────────────────────────────────────────────
// CipherState
// ─────────────────────────────────────────────────────────────────────────────

void CipherState::initializeKey(const KeyBytes& key)
{
    k_ = key;
    n_ = 0;
    hasKey_ = true;
}

void CipherState::clear()
{
    crypto_wipe(k_.data(), kKeyLen);
    n_ = 0;
    hasKey_ = false;
}

Result CipherState::encryptWithAd(ByteSpan ad, ByteSpan plaintext, ByteBuffer& out)
{
    // Before the first MixKey there is no key, and the spec says plaintext
    // passes through untouched. That is correct for the handshake's opening
    // messages, which carry only public ephemeral keys.
    if (!hasKey_) {
        out.assign(plaintext.begin(), plaintext.end());
        return Result::ok();
    }

    Byte nonce[12];
    nonceFor(n_, nonce);

    out.resize(plaintext.size() + kTagLen);

    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, k_.data(), nonce);
    crypto_aead_write(&ctx, out.data(), out.data() + plaintext.size(), ad.data(), ad.size(),
                      plaintext.data(), plaintext.size());
    crypto_wipe(&ctx, sizeof ctx);

    ++n_;
    return Result::ok();
}

Result CipherState::decryptWithAd(ByteSpan ad, ByteSpan ciphertext, ByteBuffer& out)
{
    if (!hasKey_) {
        out.assign(ciphertext.begin(), ciphertext.end());
        return Result::ok();
    }

    if (ciphertext.size() < kTagLen) {
        return Result::error(ErrorCode::DecryptFailed, "Ciphertext is shorter than its tag.");
    }

    Byte nonce[12];
    nonceFor(n_, nonce);

    const std::size_t textLen = ciphertext.size() - kTagLen;
    out.resize(textLen);

    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, k_.data(), nonce);
    const int ok = crypto_aead_read(&ctx, out.data(), ciphertext.data() + textLen, ad.data(),
                                    ad.size(), ciphertext.data(), textLen);
    crypto_wipe(&ctx, sizeof ctx);

    if (ok != 0) {
        // Do NOT advance the nonce on failure: a forged frame must not be able
        // to desynchronise the counter and break the session for both peers.
        out.clear();
        return Result::error(ErrorCode::DecryptFailed, "Authentication tag did not verify.");
    }

    ++n_;
    return Result::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// SymmetricState
// ─────────────────────────────────────────────────────────────────────────────

void SymmetricState::initialize(const char* protocolName)
{
    const std::size_t nameLen = std::strlen(protocolName);

    h_.fill(0);
    if (nameLen <= kHashLen) {
        std::memcpy(h_.data(), protocolName, nameLen);  // zero-padded
    } else {
        crypto_blake2b(h_.data(), kHashLen,
                       reinterpret_cast<const Byte*>(protocolName), nameLen);
    }
    ck_ = h_;
    cipher_.clear();
}

void SymmetricState::mixKey(ByteSpan ikm)
{
    HashBytes newCk{};
    HashBytes tempK{};
    hkdf(ck_, ikm, 2, newCk, tempK);

    ck_ = newCk;

    // HASHLEN is 64 but the cipher key is 32, so the spec truncates.
    KeyBytes key{};
    std::memcpy(key.data(), tempK.data(), kKeyLen);
    cipher_.initializeKey(key);

    crypto_wipe(tempK.data(), kHashLen);
    crypto_wipe(key.data(), kKeyLen);
}

void SymmetricState::mixHash(ByteSpan data)
{
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, kHashLen);
    crypto_blake2b_update(&ctx, h_.data(), kHashLen);
    crypto_blake2b_update(&ctx, data.data(), data.size());
    crypto_blake2b_final(&ctx, h_.data());
}

Result SymmetricState::encryptAndHash(ByteSpan plaintext, ByteBuffer& out)
{
    const Result r = cipher_.encryptWithAd(ByteSpan{h_.data(), kHashLen}, plaintext, out);
    if (r.isError()) return r;
    mixHash(out);
    return Result::ok();
}

Result SymmetricState::decryptAndHash(ByteSpan ciphertext, ByteBuffer& out)
{
    // h must be mixed with the CIPHERTEXT, and only after decrypting against the
    // pre-update h. Mixing the plaintext instead silently diverges the two sides.
    const Result r = cipher_.decryptWithAd(ByteSpan{h_.data(), kHashLen}, ciphertext, out);
    if (r.isError()) return r;
    mixHash(ciphertext);
    return Result::ok();
}

void SymmetricState::split(CipherState& sending, CipherState& receiving)
{
    HashBytes t1{};
    HashBytes t2{};
    hkdf(ck_, ByteSpan{}, 2, t1, t2);

    KeyBytes k1{};
    KeyBytes k2{};
    std::memcpy(k1.data(), t1.data(), kKeyLen);
    std::memcpy(k2.data(), t2.data(), kKeyLen);

    sending.initializeKey(k1);
    receiving.initializeKey(k2);

    crypto_wipe(t1.data(), kHashLen);
    crypto_wipe(t2.data(), kHashLen);
    crypto_wipe(k1.data(), kKeyLen);
    crypto_wipe(k2.data(), kKeyLen);
}

// ─────────────────────────────────────────────────────────────────────────────
// HandshakeState — XX
// ─────────────────────────────────────────────────────────────────────────────

Result HandshakeState::initialize(Role role, const KeyBytes& staticPublicKey,
                                  StaticDh staticDh)
{
    if (!staticDh) {
        return Result::error(ErrorCode::InvalidArgument, "No static DH callback supplied.");
    }

    role_ = role;
    s_pub_ = staticPublicKey;
    staticDh_ = std::move(staticDh);

    symmetric_.initialize(kProtocolName);
    // XX has no pre-message keys, and we use an empty prologue. Both peers must
    // agree on the prologue or the handshake fails at the first decrypt.
    symmetric_.mixHash(ByteSpan{});

    step_ = 0;
    complete_ = false;
    return Result::ok();
}

Result HandshakeState::writeMessage(ByteSpan payload, ByteBuffer& out)
{
    out.clear();

    if (role_ == Role::Initiator) {
        if (step_ == 0) return writeMessage1(payload, out);
        if (step_ == 2) return writeMessage3(payload, out);
    } else {
        if (step_ == 1) return writeMessage2(payload, out);
    }
    return Result::error(ErrorCode::HandshakeFailed, "Handshake write out of sequence.");
}

Result HandshakeState::readMessage(ByteSpan message, ByteBuffer& payloadOut)
{
    payloadOut.clear();

    if (role_ == Role::Responder) {
        if (step_ == 0) return readMessage1(message, payloadOut);
        if (step_ == 2) return readMessage3(message, payloadOut);
    } else {
        if (step_ == 1) return readMessage2(message, payloadOut);
    }
    return Result::error(ErrorCode::HandshakeFailed, "Handshake read out of sequence.");
}

// ── -> e ─────────────────────────────────────────────────────────────────────

Result HandshakeState::writeMessage1(ByteSpan payload, ByteBuffer& out)
{
    randomBytes(e_sec_.data(), kDhLen);
    crypto_x25519_public_key(e_pub_.data(), e_sec_.data());

    out.insert(out.end(), e_pub_.begin(), e_pub_.end());
    symmetric_.mixHash(ByteSpan{e_pub_.data(), kDhLen});

    ByteBuffer encrypted;
    if (const Result r = symmetric_.encryptAndHash(payload, encrypted); r.isError()) return r;
    out.insert(out.end(), encrypted.begin(), encrypted.end());

    step_ = 1;
    return Result::ok();
}

Result HandshakeState::readMessage1(ByteSpan message, ByteBuffer& payloadOut)
{
    if (message.size() < kDhLen) {
        return Result::error(ErrorCode::HandshakeFailed, "Message 1 is too short.");
    }

    std::memcpy(re_.data(), message.data(), kDhLen);
    symmetric_.mixHash(ByteSpan{re_.data(), kDhLen});

    const ByteSpan rest = message.subspan(kDhLen, message.size() - kDhLen);
    if (const Result r = symmetric_.decryptAndHash(rest, payloadOut); r.isError()) return r;

    step_ = 1;
    return Result::ok();
}

// ── <- e, ee, s, es ──────────────────────────────────────────────────────────

Result HandshakeState::writeMessage2(ByteSpan payload, ByteBuffer& out)
{
    randomBytes(e_sec_.data(), kDhLen);
    crypto_x25519_public_key(e_pub_.data(), e_sec_.data());

    out.insert(out.end(), e_pub_.begin(), e_pub_.end());
    symmetric_.mixHash(ByteSpan{e_pub_.data(), kDhLen});

    // ee — our ephemeral against theirs
    DhBytes shared{};
    crypto_x25519(shared.data(), e_sec_.data(), re_.data());
    symmetric_.mixKey(ByteSpan{shared.data(), kDhLen});

    // s — our static, now encrypted under the key ee established
    ByteBuffer encryptedStatic;
    if (const Result r = symmetric_.encryptAndHash(ByteSpan{s_pub_.data(), kDhLen},
                                                   encryptedStatic);
        r.isError()) {
        return r;
    }
    out.insert(out.end(), encryptedStatic.begin(), encryptedStatic.end());

    // es — our static against their ephemeral
    DhBytes staticShared{};
    if (!staticDh_(re_, staticShared)) {
        return Result::error(ErrorCode::HandshakeFailed, "Static DH failed.");
    }
    symmetric_.mixKey(ByteSpan{staticShared.data(), kDhLen});

    ByteBuffer encryptedPayload;
    if (const Result r = symmetric_.encryptAndHash(payload, encryptedPayload); r.isError()) {
        return r;
    }
    out.insert(out.end(), encryptedPayload.begin(), encryptedPayload.end());

    crypto_wipe(shared.data(), kDhLen);
    crypto_wipe(staticShared.data(), kDhLen);

    step_ = 2;
    return Result::ok();
}

Result HandshakeState::readMessage2(ByteSpan message, ByteBuffer& payloadOut)
{
    // ephemeral + (static + tag) + (payload + tag)
    if (message.size() < kDhLen + kDhLen + kTagLen + kTagLen) {
        return Result::error(ErrorCode::HandshakeFailed, "Message 2 is too short.");
    }

    std::size_t offset = 0;

    std::memcpy(re_.data(), message.data(), kDhLen);
    symmetric_.mixHash(ByteSpan{re_.data(), kDhLen});
    offset += kDhLen;

    // ee
    DhBytes shared{};
    crypto_x25519(shared.data(), e_sec_.data(), re_.data());
    symmetric_.mixKey(ByteSpan{shared.data(), kDhLen});

    // s
    ByteBuffer remoteStatic;
    if (const Result r = symmetric_.decryptAndHash(message.subspan(offset, kDhLen + kTagLen),
                                                   remoteStatic);
        r.isError()) {
        return r;
    }
    if (remoteStatic.size() != kDhLen) {
        return Result::error(ErrorCode::HandshakeFailed, "Remote static key has the wrong size.");
    }
    std::memcpy(rs_.data(), remoteStatic.data(), kDhLen);
    haveRemoteStatic_ = true;
    offset += kDhLen + kTagLen;

    // es — our ephemeral against their static
    DhBytes esShared{};
    crypto_x25519(esShared.data(), e_sec_.data(), rs_.data());
    symmetric_.mixKey(ByteSpan{esShared.data(), kDhLen});

    if (const Result r =
            symmetric_.decryptAndHash(message.subspan(offset, message.size() - offset),
                                      payloadOut);
        r.isError()) {
        return r;
    }

    crypto_wipe(shared.data(), kDhLen);
    crypto_wipe(esShared.data(), kDhLen);

    step_ = 2;
    return Result::ok();
}

// ── -> s, se ─────────────────────────────────────────────────────────────────

Result HandshakeState::writeMessage3(ByteSpan payload, ByteBuffer& out)
{
    ByteBuffer encryptedStatic;
    if (const Result r = symmetric_.encryptAndHash(ByteSpan{s_pub_.data(), kDhLen},
                                                   encryptedStatic);
        r.isError()) {
        return r;
    }
    out.insert(out.end(), encryptedStatic.begin(), encryptedStatic.end());

    // se — our static against their ephemeral
    DhBytes shared{};
    if (!staticDh_(re_, shared)) {
        return Result::error(ErrorCode::HandshakeFailed, "Static DH failed.");
    }
    symmetric_.mixKey(ByteSpan{shared.data(), kDhLen});

    ByteBuffer encryptedPayload;
    if (const Result r = symmetric_.encryptAndHash(payload, encryptedPayload); r.isError()) {
        return r;
    }
    out.insert(out.end(), encryptedPayload.begin(), encryptedPayload.end());

    crypto_wipe(shared.data(), kDhLen);

    // Initiator sends first on the transport, so its sending cipher is the first
    // Split() output. The responder mirrors this in readMessage3.
    symmetric_.split(sending_, receiving_);
    complete_ = true;
    step_ = 3;
    return Result::ok();
}

Result HandshakeState::readMessage3(ByteSpan message, ByteBuffer& payloadOut)
{
    if (message.size() < kDhLen + kTagLen + kTagLen) {
        return Result::error(ErrorCode::HandshakeFailed, "Message 3 is too short.");
    }

    ByteBuffer remoteStatic;
    if (const Result r = symmetric_.decryptAndHash(message.subspan(0, kDhLen + kTagLen),
                                                   remoteStatic);
        r.isError()) {
        return r;
    }
    if (remoteStatic.size() != kDhLen) {
        return Result::error(ErrorCode::HandshakeFailed, "Remote static key has the wrong size.");
    }
    std::memcpy(rs_.data(), remoteStatic.data(), kDhLen);
    haveRemoteStatic_ = true;

    // se — our ephemeral against their static
    DhBytes shared{};
    crypto_x25519(shared.data(), e_sec_.data(), rs_.data());
    symmetric_.mixKey(ByteSpan{shared.data(), kDhLen});

    const std::size_t offset = kDhLen + kTagLen;
    if (const Result r =
            symmetric_.decryptAndHash(message.subspan(offset, message.size() - offset),
                                      payloadOut);
        r.isError()) {
        return r;
    }

    crypto_wipe(shared.data(), kDhLen);

    // Mirrored: the responder RECEIVES on the initiator's sending cipher.
    symmetric_.split(receiving_, sending_);
    complete_ = true;
    step_ = 3;
    return Result::ok();
}

bool HandshakeState::remoteStaticKey(KeyBytes& out) const
{
    if (!haveRemoteStatic_) return false;
    out = rs_;
    return true;
}

}  // namespace anytime::crypto::noise
