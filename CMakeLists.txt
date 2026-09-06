cmake_minimum_required(VERSION 3.21)

# Single source of truth for the app version. Android manifest, the C++ layer and
# the in-app settings screen all derive from this.
project(AnyTimeChat VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Headers are included as "core/...", "adapters/..." from the repository root.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ─────────────────────────────────────────────────────────────────────────────
# BUILD STATE — the project is mid-scaffold (docs/SKELETON.md).
#
#   ANYTIME_BUILD_APP=OFF (default)  core/ only. Configures and builds today.
#   ANYTIME_BUILD_APP=ON             adds adapters + the Qt app. Needs the
#                                    phase 0 .cpp files listed further down.
#
# Flip it on once adapters/ has real sources. Until then the app target would
# fail to link, so it is off by default rather than broken by default.
# ─────────────────────────────────────────────────────────────────────────────

option(ANYTIME_BUILD_APP   "Build the Qt shell and adapters" ON)
option(ANYTIME_BUILD_TESTS "Build the core test suite"                               ON)

# ─────────────────────────────────────────────────────────────────────────────
# Qt modules
#
# LICENCE GATE (docs/ARCHITECTURE.md section 03): Qt HTTP Server is
# GPLv3-or-commercial with no LGPL option, so linking it would relicense this
# whole application. We hand-roll the router on QTcpServer instead — hence
# Network, not HttpServer. Qt Bluetooth is absent by design: the radio is native
# per platform (AD-02).
# ─────────────────────────────────────────────────────────────────────────────

# Mobile runs the UI in the platform WebView. Desktop runs HEADLESS and prints
# its URL for an ordinary browser to open — Qt WebEngine is not part of the
# standard desktop kit, and browser-based iteration is the faster loop anyway.
# Two headless instances plus two browser tabs is how Nearby is exercised
# without a radio (AD-07).
if(ANYTIME_BUILD_APP)
    if(ANDROID OR IOS)
        find_package(Qt6 REQUIRED COMPONENTS Core Network Quick WebView)
        set(ANYTIME_USE_WEBVIEW ON)
    else()
        find_package(Qt6 REQUIRED COMPONENTS Core Network Gui)
        set(ANYTIME_USE_WEBVIEW OFF)
    endif()
    qt_standard_project_setup(REQUIRES 6.8)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# core — pure C++17. Deliberately NOT linked against Qt.
#
# This target must stay Qt-free so it can be unit-tested with no event loop and
# no device. scripts/check-core-includes.sh enforces it; wire that into CI now,
# while core/ is still clean (docs/ARCHITECTURE.md section 04).
# ─────────────────────────────────────────────────────────────────────────────

set(ANYTIME_CORE_HEADERS
    core/AppCore.h

    # domain — layered bottom-up; Types.h is the umbrella everyone else includes
    core/domain/Primitives.h
    core/domain/Enums.h
    core/domain/Identifiers.h
    core/domain/GlobalStructures.h
    core/domain/Types.h
    core/domain/Result.h
    core/domain/Entities.h

    # constants — the system's tuning surface, grouped by role
    core/domain/constants/CryptoConstants.h
    core/domain/constants/DeliveryConstants.h
    core/domain/constants/DiscoveryConstants.h
    core/domain/constants/NetworkConstants.h
    core/domain/constants/ProtocolConstants.h
    core/domain/constants/RequestConstants.h

    core/ports/IClock.h
    core/ports/IEventSink.h
    core/ports/IIdentityStore.h
    core/ports/IRepositories.h
    core/ports/ISecureSession.h
    core/ports/ITransport.h
    core/protocol/AppEnvelope.h
    core/protocol/DedupCache.h
    core/protocol/Fragmenter.h
    core/protocol/Packet.h
    core/protocol/PacketCodec.h
    core/services/AccountService.h
    core/services/ChatService.h
    core/services/DeliveryService.h
    core/services/DiscoveryService.h
    core/services/MeshEngine.h
    core/services/RequestService.h
    core/services/RoutingPolicy.h
    core/services/SessionManager.h
    core/services/TransportRegistry.h
)

set(ANYTIME_CORE_SOURCES
    core/domain/Enums.cpp
    core/domain/Identifiers.cpp
    core/domain/Entities.cpp

    core/protocol/PacketCodec.cpp
    core/protocol/DedupCache.cpp
    core/protocol/Fragmenter.cpp

    core/services/TransportRegistry.cpp
    core/services/RoutingPolicy.cpp
    core/services/SessionManager.cpp
    core/services/MeshEngine.cpp
    core/services/AccountService.cpp
    core/services/DiscoveryService.cpp
    core/AppCore.cpp

    # Compiles every header above, so declarations are validated from day one
    # even before their implementations exist. See the file for why.
    core/HeaderCheck.cpp
)

# ── PHASE 0 / 1 WORK QUEUE ───────────────────────────────────────────────────
# Uncomment each line as you write the file. Suggested order is in
# docs/SKELETON.md; PacketCodec first, then the loopback transport.
#
#   core/services/RequestService.cpp
#   core/services/ChatService.cpp
#   core/services/DeliveryService.cpp

add_library(anytime_core STATIC
    ${ANYTIME_CORE_SOURCES}
    ${ANYTIME_CORE_HEADERS}   # listed so the IDE shows them in the project tree
)
target_include_directories(anytime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(anytime_core PROPERTIES AUTOMOC OFF)  # no Qt in core/

# ─────────────────────────────────────────────────────────────────────────────
# adapters + app — everything platform-shaped. May use Qt freely.
# ─────────────────────────────────────────────────────────────────────────────

if(ANYTIME_BUILD_APP)

    set(ANYTIME_ADAPTER_HEADERS
        adapters/clock/QtClock.h
        adapters/codec/QtCborCodec.h
        adapters/crypto/NoiseSessionFactory.h
        adapters/http/LocalHttpServer.h
        adapters/http/SseHub.h
        adapters/http/HttpTypes.h
        adapters/http/Router.h
        adapters/http/ApiRoutes.h
        adapters/http/JsonMapping.h
        adapters/identity/DevIdentityStore.h
        adapters/persistence/settings/SettingsStore.h
        adapters/transport/loopback/LoopbackTransport.h
    )

    # These do not exist yet — write them, then flip ANYTIME_BUILD_APP on.
    set(ANYTIME_ADAPTER_SOURCES
        adapters/clock/QtClock.cpp
        adapters/codec/QtCborCodec.cpp
        adapters/http/HttpTypes.cpp
        adapters/http/Router.cpp
        adapters/http/LocalHttpServer.cpp
        adapters/http/SseHub.cpp
        adapters/http/ApiRoutes.cpp
        adapters/http/JsonMapping.cpp
        adapters/identity/DevIdentityStore.cpp
        adapters/persistence/settings/SettingsStore.cpp
        adapters/transport/loopback/LoopbackTransport.cpp
    )

    if(ANDROID)
        list(APPEND ANYTIME_ADAPTER_HEADERS adapters/transport/ble/BleGattTransport.h)
        list(APPEND ANYTIME_ADAPTER_SOURCES
            adapters/transport/ble/BleGattTransport.cpp
            adapters/transport/ble/BleJniBridge.cpp
        )
    endif()

    add_library(anytime_adapters STATIC
        ${ANYTIME_ADAPTER_SOURCES}
        ${ANYTIME_ADAPTER_HEADERS}
    )
    target_link_libraries(anytime_adapters
        PUBLIC anytime_core Qt6::Core Qt6::Network
    )

    qt_add_executable(anytime app/main.cpp)

    target_compile_definitions(anytime PRIVATE
        ANYTIME_VERSION="${PROJECT_VERSION}"
        ANYTIME_UI_DIST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui/dist"
    )

    if(ANYTIME_USE_WEBVIEW)
        target_compile_definitions(anytime PRIVATE ANYTIME_USE_WEBVIEW)
        qt_add_qml_module(anytime
            URI AnyTime VERSION 1.0
            QML_FILES app/MainWebView.qml
        )
        target_link_libraries(anytime PRIVATE Qt6::WebView Qt6::Quick)
    else()
        target_link_libraries(anytime PRIVATE Qt6::Gui)
    endif()

    target_link_libraries(anytime PRIVATE anytime_adapters)

    # Release builds must never allow the file-backed identity fallback the
    # desktop dev path uses — see adapters/identity/KeystoreIdentityStore.h.
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_definitions(anytime PRIVATE ANYTIME_FORBID_INSECURE_IDENTITY)
    endif()

    if(ANDROID)
        set_property(TARGET anytime APPEND PROPERTY
            QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/platform/android")
        set_property(TARGET anytime PROPERTY
            QT_ANDROID_PACKAGE_NAME "org.anytimechat.app")

        math(EXPR ANYTIME_VERSION_CODE
             "${PROJECT_VERSION_MAJOR} * 100 + ${PROJECT_VERSION_MINOR}")
        set_property(TARGET anytime PROPERTY QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}")
        set_property(TARGET anytime PROPERTY QT_ANDROID_VERSION_CODE "${ANYTIME_VERSION_CODE}")
    endif()

endif()  # ANYTIME_BUILD_APP

# ─────────────────────────────────────────────────────────────────────────────
# tests — GoogleTest against core/ with fakes. No Qt, no device, no radio.
# ─────────────────────────────────────────────────────────────────────────────

if(ANYTIME_BUILD_TESTS AND NOT ANDROID AND NOT IOS
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

# ─────────────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "AnyTime Chat ${PROJECT_VERSION} — scaffold state")
message(STATUS "  core library ....... building (domain + header check)")
if(ANYTIME_BUILD_APP)
    message(STATUS "  adapters + app ..... ENABLED")
else()
    message(STATUS "  adapters + app ..... off  (-DANYTIME_BUILD_APP=ON once adapters/*.cpp exist)")
endif()
message(STATUS "  next up ............ see docs/SKELETON.md 'Build order'")
message(STATUS "")
