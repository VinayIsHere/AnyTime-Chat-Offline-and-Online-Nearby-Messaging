// tests/IdentityTests.cpp
//
// These tests exist to answer one question: CAN A SIGNATURE BE FORGED?
//
// The store this replaced could be forged trivially — its "signatures" were a
// keyed hash over the public key, so anyone who could verify could also sign.
// Announce authenticity was decorative, and nothing in the test suite said so.
//
// Every case below fails loudly against that old implementation and passes
// against real Ed25519.

#include "tests/TestHarness.h"

#include <QDir>
#include <QFile>

#include "adapters/identity/MonocypherIdentityStore.h"

namespace anytime::test {

namespace {

QString scratchPath(const char* name)
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

}  // namespace

void runIdentityTests(Registry& r)
{
    r.section("Identity — key generation and derivation");

    const QString pathA = scratchPath("a.key");
    const QString pathB = scratchPath("b.key");
    QFile::remove(pathA);
    QFile::remove(pathB);

    identity::MonocypherIdentityStore storeA(pathA);
    identity::MonocypherIdentityStore storeB(pathB);

    auto identityA = storeA.loadOrCreate();
    auto identityB = storeB.loadOrCreate();

    r.check(identityA.isOk(), "generates an identity on first use");
    r.check(identityB.isOk(), "generates a second, independent identity");
    r.check(!identityA.value().peerId.isNull(), "peer id is not null");
    r.check(identityA.value().peerId != identityB.value().peerId,
            "two identities get different peer ids");
    r.check(identityA.value().staticPublicKey != identityB.value().staticPublicKey,
            "two identities get different static keys");
    r.check(identityA.value().peerId.toHex().size() == 16, "peer id is 16 hex characters");

    {
        // Reload from disk must reproduce the same identity, or every restart
        // would look like a different person to every peer.
        identity::MonocypherIdentityStore reopened(pathA);
        auto again = reopened.loadOrCreate();
        r.check(again.isOk() && again.value().peerId == identityA.value().peerId,
                "identity survives a reload");
        r.check(again.value().staticPublicKey == identityA.value().staticPublicKey,
                "static key survives a reload");
    }

    r.section("Identity — signatures are real");

    const ByteBuffer message = bytesOf("announce: vinay");
    auto signature = storeA.sign(message);
    r.check(signature.isOk(), "signs a message");

    r.check(storeA.verify(message, signature.value(), identityA.value().signingPublicKey),
            "a genuine signature verifies");

    {
        // THE forgery test. B tries to pass its own signature off as A's.
        auto forged = storeB.sign(message);
        r.check(!storeA.verify(message, forged.value(), identityA.value().signingPublicKey),
                "another identity's signature is REJECTED");
    }

    {
        // Anyone can see the public key; that must not be enough to sign.
        Signature guessed{};
        for (std::size_t i = 0; i < kSignatureBytes; ++i) {
            guessed[i] = identityA.value().signingPublicKey[i % kPublicKeyBytes];
        }
        r.check(!storeA.verify(message, guessed, identityA.value().signingPublicKey),
                "a signature derived from the public key is REJECTED");
    }

    {
        Signature tampered = signature.value();
        tampered[0] ^= 0x01;
        r.check(!storeA.verify(message, tampered, identityA.value().signingPublicKey),
                "a one-bit change to the signature is REJECTED");
    }

    {
        const ByteBuffer altered = bytesOf("announce: vinay!");
        r.check(!storeA.verify(altered, signature.value(), identityA.value().signingPublicKey),
                "a modified message is REJECTED");
    }

    {
        r.check(!storeA.verify(message, signature.value(), identityB.value().signingPublicKey),
                "verifying against the wrong public key is REJECTED");
    }

    r.section("Identity — X25519 agreement");

    {
        auto sharedAB = storeA.agree(identityB.value().staticPublicKey);
        auto sharedBA = storeB.agree(identityA.value().staticPublicKey);

        r.check(sharedAB.isOk() && sharedBA.isOk(), "both sides compute a shared secret");
        r.check(sharedAB.value() == sharedBA.value(),
                "both sides agree on the SAME shared secret");

        bool nonZero = false;
        for (const Byte b : sharedAB.value()) {
            if (b != 0) { nonZero = true; break; }
        }
        r.check(nonZero, "shared secret is not all zeroes");
    }

    {
        // An all-zero public key is a low-order point; agreeing with it yields a
        // zero shared secret, which must be refused rather than used as a key.
        PublicKey degenerate{};
        r.check(storeA.agree(degenerate).isError(),
                "a degenerate public key is REJECTED");
    }

    r.section("Identity — wipe");

    {
        r.check(storeB.wipe().isOk(), "wipe succeeds");
        r.check(!QFile::exists(pathB), "wipe removes the key file");
    }

    QFile::remove(pathA);
}

}  // namespace anytime::test
