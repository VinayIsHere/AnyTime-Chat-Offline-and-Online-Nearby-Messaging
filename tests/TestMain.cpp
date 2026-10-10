// tests/TestMain.cpp

#include "tests/TestHarness.h"

#include <QCoreApplication>

#include <spdlog/spdlog.h>

namespace anytime::test {
void runPacketCodecTests(Registry&);
void runDedupCacheTests(Registry&);
void runIdentityTests(Registry&);
void runFragmenterTests(Registry&);
void runNoiseTests(Registry&);
void runSqliteStoreTests(Registry&);
void runMediaEnvelopeTests(Registry&);
void runRoutingPolicyTests(Registry&);
void runPeerPresentationTests(Registry&);
void runServiceTests(Registry&);
}  // namespace anytime::test

int main(int argc, char* argv[])
{
    // The identity store uses QFile and QCryptographicHash, so the tests need a
    // Qt application object even though core/ itself is Qt-free.
    QCoreApplication app(argc, argv);

    // Tests deliberately drive failure paths — bad signatures, broken handshakes —
    // and each one logs a warning. That noise would bury the pass/fail report.
    spdlog::set_level(spdlog::level::off);

    anytime::test::Registry registry;

    std::printf("\n  AnyTime Chat — core tests\n");

    anytime::test::runPacketCodecTests(registry);
    anytime::test::runFragmenterTests(registry);
    anytime::test::runDedupCacheTests(registry);
    anytime::test::runIdentityTests(registry);
    anytime::test::runNoiseTests(registry);
    anytime::test::runSqliteStoreTests(registry);
    anytime::test::runMediaEnvelopeTests(registry);
    anytime::test::runRoutingPolicyTests(registry);
    anytime::test::runPeerPresentationTests(registry);
    anytime::test::runServiceTests(registry);

    return registry.report();
}
