# tests/CMakeLists.txt
#
# Runs against core/ plus the identity adapter. No device, no radio, no network:
# these are the parts that must be correct before anything is worth testing on
# hardware.

find_package(Qt6 REQUIRED COMPONENTS Core Sql)

add_executable(anytime_tests
    TestMain.cpp
    PacketCodecTests.cpp
    FragmenterTests.cpp
    DedupCacheTests.cpp
    IdentityTests.cpp
    NoiseTests.cpp
    SqliteStoreTests.cpp
    MediaEnvelopeTests.cpp
    RoutingPolicyTests.cpp
    PeerPresentationTests.cpp
    ServiceTests.cpp
    LoggingTests.cpp
    TestHarness.h
)

target_link_libraries(anytime_tests
    PRIVATE anytime_core monocypher Qt6::Core Qt6::Sql
)

# IdentityTests needs the adapter's .cpp directly rather than the whole adapters
# library, which would drag in the HTTP server and every transport.
target_sources(anytime_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/adapters/identity/MonocypherIdentityStore.cpp
    ${CMAKE_SOURCE_DIR}/adapters/crypto/NoiseProtocol.cpp
    ${CMAKE_SOURCE_DIR}/adapters/crypto/NoiseSessionFactory.cpp
    ${CMAKE_SOURCE_DIR}/adapters/persistence/sqlite/SqliteStore.cpp
    ${CMAKE_SOURCE_DIR}/adapters/persistence/sqlite/MonocypherDataCipher.cpp
    ${CMAKE_SOURCE_DIR}/adapters/codec/QtCborCodec.cpp
    ${CMAKE_SOURCE_DIR}/adapters/notify/Notifier.cpp
    ${CMAKE_SOURCE_DIR}/adapters/logging/LogSetup.cpp
)

set_target_properties(anytime_tests PROPERTIES AUTOMOC OFF)

add_test(NAME core_tests COMMAND anytime_tests)
