// tests/NoiseTests.cpp
//
// Noise_XX_25519_ChaChaPoly_BLAKE2b.
//
// The primitives are Monocypher's and are not retested here. What IS tested is
// everything the state machine has to get right, because a handshake that
// "works" can still be silently broken: two sides that derive different keys
// fail loudly, but two sides that skip a MixKey would appear to work while
// throwing away authentication.
//
// The properties asserted below are the ones whose absence would not otherwise
// be visible:
//   · both sides derive the SAME transport keys, in the right direction
//   · each side learns the other's REAL static key (that value gets pinned)
//   · a tampered handshake message aborts rather than downgrading
//   · a tampered transport frame fails to decrypt AND does not desynchronise
//   · two independent handshakes produce different keys (ephemerals are used)

#include "tests/TestHarness.h"

#include <QDir>
#include <QFile>

#include "adapters/crypto/NoiseProtocol.h"
#include "adapters/crypto/NoiseSessionFactory.h"
#include "adapters/identity/MonocypherIdentityStore.h"
#include "core/protocol/Packet.h"

namespace anytime::test {

using namespace anytime::crypto::noise;

namespace {

QString scratch(const char* name)
{
    const QString dir = QDir::tempPath() + QStringLiteral("/anytime-tests");
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + QLatin1String(name);
}

ByteBuffer bytesOf(const char* text)
{
    ByteBuffer out;
    for (const char* p = text; *p; ++p) out.push_back(static_cast<Byte>(*p));
    return out;
}

/// Drives a full XX handshake between two live sessions.
bool completeHandshake(ISecureSession& initiator, ISecureSession& responder)
{
    ByteBuffer msg1, msg2, msg3, ignored;

    if (initiator.beginHandshake(HandshakeRole::Initiator, msg1).isError()) return false;
    if (responder.beginHandshake(HandshakeRole::Responder, ignored).isError()) return false;

    if (responder.readHandshake(msg1, msg2).isError()) return false;  // -> message 2
    if (initiator.readHandshake(msg2, msg3).isError()) return false;  // -> message 3
    if (responder.readHandshake(msg3, ignored).isError()) return false;

    return initiator.isEstablished() && responder.isEstablished();
}

}  // namespace

void runNoiseTests(Registry& r)
{
    r.section("Noise — HKDF and HMAC shape");

    {
        HashBytes ck{};
        ck.fill(0x01);
        const ByteBuffer ikm = bytesOf("input key material");

        HashBytes o1{}, o2{}, o3{};
        hkdf(ck, ikm, 3, o1, o2, &o3);

        r.check(o1 != o2 && o2 != o3 && o1 != o3, "HKDF outputs differ from each other");

        HashBytes p1{}, p2{}, p3{};
        hkdf(ck, ikm, 3, p1, p2, &p3);
        r.check(o1 == p1 && o2 == p2 && o3 == p3, "HKDF is deterministic");

        HashBytes q1{}, q2{};
        HashBytes other{};
        other.fill(0x02);
        hkdf(other, ikm, 2, q1, q2);
        r.check(q1 != o1, "a different chaining key gives different output");
    }

    r.section("Noise — CipherState");

    {
        CipherState cipher;
        KeyBytes key{};
        key.fill(0x42);
        cipher.initializeKey(key);

        const ByteBuffer plaintext = bytesOf("the quick brown fox");
        ByteBuffer ciphertext;
        r.check(cipher.encryptWithAd(ByteSpan{}, plaintext, ciphertext).isOk(), "encrypts");
        r.check(ciphertext.size() == plaintext.size() + kTagLen, "adds a 16-byte tag");
        r.check(ciphertext != plaintext, "ciphertext differs from plaintext");

        CipherState reader;
        reader.initializeKey(key);
        ByteBuffer recovered;
        r.check(reader.decryptWithAd(ByteSpan{}, ciphertext, recovered).isOk(), "decrypts");
        r.check(recovered == plaintext, "round trip preserves the plaintext");
    }

    {
        // The nonce must advance, or two identical messages would encrypt
        // identically and leak that they are the same.
        CipherState cipher;
        KeyBytes key{};
        key.fill(0x07);
        cipher.initializeKey(key);

        const ByteBuffer message = bytesOf("same message");
        ByteBuffer first, second;
        cipher.encryptWithAd(ByteSpan{}, message, first);
        cipher.encryptWithAd(ByteSpan{}, message, second);
        r.check(first != second, "the same plaintext encrypts differently each time");
    }

    {
        // A forged frame must fail AND leave the counter untouched, or one
        // injected packet would break the session for both peers.
        CipherState writer, reader;
        KeyBytes key{};
        key.fill(0x5A);
        writer.initializeKey(key);
        reader.initializeKey(key);

        const ByteBuffer m1 = bytesOf("first");
        const ByteBuffer m2 = bytesOf("second");
        ByteBuffer c1, c2;
        writer.encryptWithAd(ByteSpan{}, m1, c1);
        writer.encryptWithAd(ByteSpan{}, m2, c2);

        ByteBuffer forged = c1;
        forged[0] ^= 0x01;
        ByteBuffer out;
        r.check(reader.decryptWithAd(ByteSpan{}, forged, out).isError(),
                "a tampered frame is REJECTED");

        r.check(reader.decryptWithAd(ByteSpan{}, c1, out).isOk() && out == m1,
                "the genuine frame still decrypts after a forgery attempt");
        r.check(reader.decryptWithAd(ByteSpan{}, c2, out).isOk() && out == m2,
                "and the session stays in sync");
    }

    r.section("Noise — XX handshake");

    const QString pathA = scratch("noise-a.key");
    const QString pathB = scratch("noise-b.key");
    QFile::remove(pathA);
    QFile::remove(pathB);

    identity::MonocypherIdentityStore storeA(pathA);
    identity::MonocypherIdentityStore storeB(pathB);
    auto idA = storeA.loadOrCreate();
    auto idB = storeB.loadOrCreate();

    crypto::NoiseSessionFactory factoryA(storeA);
    crypto::NoiseSessionFactory factoryB(storeB);

    {
        auto a = factoryA.createSession(nullptr);
        auto b = factoryB.createSession(nullptr);
        r.check(a.isOk() && b.isOk(), "factory creates sessions");

        auto initiator = std::move(a).value();
        auto responder = std::move(b).value();

        r.check(completeHandshake(*initiator, *responder), "XX handshake completes");

        // Each side must end up holding the OTHER's real static key. This is the
        // value pinned onto a Contact, so a wrong key here would pin the wrong
        // identity and defeat the whole request-gate model.
        auto seenByInitiator = initiator->remoteStaticKey();
        auto seenByResponder = responder->remoteStaticKey();

        r.check(seenByInitiator.isOk() && seenByResponder.isOk(),
                "both sides learn a remote static key");
        r.check(seenByInitiator.isOk()
                    && seenByInitiator.value() == idB.value().staticPublicKey,
                "initiator learns the responder's REAL static key");
        r.check(seenByResponder.isOk()
                    && seenByResponder.value() == idA.value().staticPublicKey,
                "responder learns the initiator's REAL static key");

        // Transport, both directions.
        const ByteBuffer hello = bytesOf("hello from the initiator");
        ByteBuffer sealed, opened;
        r.check(initiator->encrypt(hello, sealed).isOk(), "initiator encrypts");
        r.check(responder->decrypt(sealed, opened).isOk() && opened == hello,
                "responder decrypts it — the two sides agreed on keys");

        const ByteBuffer reply = bytesOf("hello back from the responder");
        ByteBuffer sealedReply, openedReply;
        r.check(responder->encrypt(reply, sealedReply).isOk(), "responder encrypts");
        r.check(initiator->decrypt(sealedReply, openedReply).isOk() && openedReply == reply,
                "initiator decrypts it — the directions are not crossed");

        // Several messages in a row, to catch a nonce that fails to advance in
        // lockstep between the two sides.
        bool streamOk = true;
        for (int i = 0; i < 20; ++i) {
            const ByteBuffer m = bytesOf("streamed message");
            ByteBuffer c, p;
            if (initiator->encrypt(m, c).isError() || responder->decrypt(c, p).isError()
                || p != m) {
                streamOk = false;
                break;
            }
        }
        r.check(streamOk, "20 consecutive messages stay in sync");
    }

    {
        // Two handshakes between the SAME identities must not produce the same
        // keys, or the ephemeral half is being ignored and forward secrecy is gone.
        auto a1 = factoryA.createSession(nullptr);
        auto b1 = factoryB.createSession(nullptr);
        auto i1 = std::move(a1).value();
        auto r1 = std::move(b1).value();
        completeHandshake(*i1, *r1);

        auto a2 = factoryA.createSession(nullptr);
        auto b2 = factoryB.createSession(nullptr);
        auto i2 = std::move(a2).value();
        auto r2 = std::move(b2).value();
        completeHandshake(*i2, *r2);

        const ByteBuffer message = bytesOf("identical plaintext");
        ByteBuffer c1, c2;
        i1->encrypt(message, c1);
        i2->encrypt(message, c2);
        r.check(c1 != c2, "a second handshake derives different keys (ephemerals are used)");

        // Cross-session decryption must fail: session 2 cannot read session 1.
        ByteBuffer out;
        r.check(r2->decrypt(c1, out).isError(),
                "one session cannot decrypt another session's traffic");
    }

    r.section("Noise — padded messages hide their length");

    {
        // What a listener actually sees: ciphertext. Padding is only real if two
        // messages of different lengths encrypt to the same number of bytes.
        auto a = factoryA.createSession(nullptr);
        auto b = factoryB.createSession(nullptr);
        auto initiator = std::move(a).value();
        auto responder = std::move(b).value();
        completeHandshake(*initiator, *responder);

        const ByteBuffer yes = bytesOf("y");
        const ByteBuffer paragraph = bytesOf(
            "A much longer reply that says a great deal more than one letter does, "
            "and would stand out at a glance if its length were visible on the air.");

        ByteBuffer rawShort, rawLong;
        initiator->encrypt(yes, rawShort);
        initiator->encrypt(paragraph, rawLong);
        r.check(rawShort.size() != rawLong.size(),
                "control: unpadded, the two replies are distinguishable by size");

        ByteBuffer paddedShort, paddedLong;
        initiator->encrypt(protocol::padPlaintext(yes), paddedShort);
        initiator->encrypt(protocol::padPlaintext(paragraph), paddedLong);
        r.check(paddedShort.size() == paddedLong.size(),
                "padded, a one-letter reply and a paragraph are the same size on the air");

        // Decrypt all four in order — the counter must stay in step.
        ByteBuffer ignored, opened, text;
        responder->decrypt(rawShort, ignored);
        responder->decrypt(rawLong, ignored);
        r.check(responder->decrypt(paddedShort, opened).isOk()
                    && protocol::unpadPlaintext(opened, text).isOk() && text == yes,
                "the far side decrypts and unpads the short reply exactly");
        r.check(responder->decrypt(paddedLong, opened).isOk()
                    && protocol::unpadPlaintext(opened, text).isOk() && text == paragraph,
                "and the long one");
    }

    r.section("Noise — rejects tampering");

    {
        auto a = factoryA.createSession(nullptr);
        auto b = factoryB.createSession(nullptr);
        auto initiator = std::move(a).value();
        auto responder = std::move(b).value();

        ByteBuffer msg1, msg2, ignored;
        initiator->beginHandshake(HandshakeRole::Initiator, msg1);
        responder->beginHandshake(HandshakeRole::Responder, ignored);
        responder->readHandshake(msg1, msg2);

        // Corrupt the responder's encrypted static key. The initiator must abort
        // rather than proceed with an unauthenticated peer.
        ByteBuffer tampered = msg2;
        tampered[kDhLen + 4] ^= 0x01;

        ByteBuffer msg3;
        r.check(initiator->readHandshake(tampered, msg3).isError(),
                "a tampered handshake message ABORTS the handshake");
        r.check(!initiator->isEstablished(), "and the session is not established");
    }

    {
        auto a = factoryA.createSession(nullptr);
        auto b = factoryB.createSession(nullptr);
        auto initiator = std::move(a).value();
        auto responder = std::move(b).value();
        completeHandshake(*initiator, *responder);

        const ByteBuffer message = bytesOf("a message worth forging");
        ByteBuffer sealed;
        initiator->encrypt(message, sealed);

        ByteBuffer tampered = sealed;
        tampered[2] ^= 0x80;

        ByteBuffer out;
        r.check(responder->decrypt(tampered, out).isError(),
                "a tampered transport frame is REJECTED");
        r.check(out.empty(), "and yields no plaintext");
    }

    {
        // Out-of-sequence handshake calls must be refused rather than
        // half-initialising a session.
        auto a = factoryA.createSession(nullptr);
        auto session = std::move(a).value();

        ByteBuffer out;
        const ByteBuffer garbage = bytesOf("not a handshake message at all");
        r.check(session->readHandshake(garbage, out).isError(),
                "reading before beginHandshake is REJECTED");

        session->beginHandshake(HandshakeRole::Initiator, out);
        ByteBuffer second;
        r.check(session->beginHandshake(HandshakeRole::Initiator, second).isError(),
                "beginning a handshake twice is REJECTED");
    }

    {
        auto a = factoryA.createSession(nullptr);
        auto session = std::move(a).value();
        ByteBuffer out;
        const ByteBuffer plaintext = bytesOf("too early");
        r.check(session->encrypt(plaintext, out).isError(),
                "encrypting before the handshake completes is REJECTED");
    }

    QFile::remove(pathA);
    QFile::remove(pathB);
}

}  // namespace anytime::test
