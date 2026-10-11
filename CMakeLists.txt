cmake_minimum_required(VERSION 3.21)

# Single source of truth for the app version. Android manifest, the C++ layer and
# the in-app settings screen all derive from this.
project(AnyTimeChat VERSION 0.2 LANGUAGES C CXX)

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
        find_package(Qt6 REQUIRED COMPONENTS Core Network Quick WebView Sql)
        set(ANYTIME_USE_WEBVIEW ON)
    else()
        find_package(Qt6 REQUIRED COMPONENTS Core Network Gui Sql)
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
    core/log/Log.h

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
    core/ports/IRadioController.h
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
    core/services/BlockService.h
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
    core/services/RequestService.cpp
    core/services/ChatService.cpp
    core/services/BlockService.cpp
    core/services/DeliveryService.cpp
    core/AppCore.cpp

    # Compiles every header above, so declarations are validated from day one
    # even before their implementations exist. See the file for why.
    core/HeaderCheck.cpp
)

# ── PHASE 0 / 1 WORK QUEUE ───────────────────────────────────────────────────
# Uncomment each line as you write the file. Suggested order is in
# docs/SKELETON.md; PacketCodec first, then the loopback transport.
#

# ─────────────────────────────────────────────────────────────────────────────
# third_party/monocypher — X25519, Ed25519, ChaCha20-Poly1305, BLAKE2b
#
# Vendored (BSD-2-Clause / CC-0) rather than fetched: cross-compiling a crypto
# library for the NDK is its own project, and Monocypher is explicitly designed
# to be dropped into a source tree. Built as C; its warnings are left alone
# because it is not our code to tidy.
#
# Defined before anytime_core so both the app and tests/ can link it.
# ─────────────────────────────────────────────────────────────────────────────

add_library(monocypher STATIC
    third_party/monocypher/monocypher.c
    third_party/monocypher/monocypher-ed25519.c
)
set_target_properties(monocypher PROPERTIES C_STANDARD 99 AUTOMOC OFF)
target_include_directories(monocypher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ─────────────────────────────────────────────────────────────────────────────
# third_party/spdlog — logging (MIT), vendored v1.17.0
#
# Built as a compiled library rather than header-only: header-only spdlog
# compiles fmt into every translation unit that logs, which is most of them.
#
# THE LOG LEVEL IS DECIDED HERE, AT COMPILE TIME. SPDLOG_ACTIVE_LEVEL removes
# every call below it from the binary — a Release build does not skip debug
# lines at runtime, it does not contain them, so debug logging costs a shipped
# build nothing.
#
#   Debug          debug     debug, info, warn, error, critical
#   anything else  ANYTIME_RELEASE_LOG_LEVEL (default: critical)
#
# Output goes to the console (logcat on Android) AND a rotating log file; see
# adapters/logging/LogSetup.cpp.
# ─────────────────────────────────────────────────────────────────────────────

set(ANYTIME_RELEASE_LOG_LEVEL "critical" CACHE STRING
    "Lowest log level compiled into non-Debug builds")
set(ANYTIME_LOG_LEVELS trace debug info warn error critical off)
set_property(CACHE ANYTIME_RELEASE_LOG_LEVEL PROPERTY STRINGS ${ANYTIME_LOG_LEVELS})

# spdlog's numbering matches this list's order: trace = 0 ... off = 6.
list(FIND ANYTIME_LOG_LEVELS "${ANYTIME_RELEASE_LOG_LEVEL}" ANYTIME_RELEASE_LOG_LEVEL_NUM)
if(ANYTIME_RELEASE_LOG_LEVEL_NUM EQUAL -1)
    message(FATAL_ERROR
        "ANYTIME_RELEASE_LOG_LEVEL must be one of: ${ANYTIME_LOG_LEVELS}")
endif()

find_package(Threads REQUIRED)

add_library(spdlog_vendored STATIC
    third_party/spdlog/src/spdlog.cpp
    third_party/spdlog/src/stdout_sinks.cpp
    third_party/spdlog/src/color_sinks.cpp
    third_party/spdlog/src/file_sinks.cpp
    third_party/spdlog/src/async.cpp
    third_party/spdlog/src/cfg.cpp
    third_party/spdlog/src/bundled_fmtlib_format.cpp
)
# SYSTEM: warnings inside spdlog and its bundled fmt are not ours to fix.
target_include_directories(spdlog_vendored SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include)
target_compile_definitions(spdlog_vendored PUBLIC
    SPDLOG_COMPILED_LIB
    SPDLOG_ACTIVE_LEVEL=$<IF:$<CONFIG:Debug>,1,${ANYTIME_RELEASE_LOG_LEVEL_NUM}>
)
target_link_libraries(spdlog_vendored PUBLIC Threads::Threads)
if(ANDROID)
    target_link_libraries(spdlog_vendored PUBLIC log)  # android_sink writes to logcat
endif()
set_target_properties(spdlog_vendored PROPERTIES AUTOMOC OFF)

add_library(anytime_core STATIC
    ${ANYTIME_CORE_SOURCES}
    ${ANYTIME_CORE_HEADERS}   # listed so the IDE shows them in the project tree
)
target_include_directories(anytime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(anytime_core PROPERTIES AUTOMOC OFF)  # no Qt in core/

# PUBLIC: anything linking the core — adapters, app, tests — gets the same
# compiled log level, so one binary never mixes two.
target_link_libraries(anytime_core PUBLIC spdlog_vendored)

# ─────────────────────────────────────────────────────────────────────────────
# adapters + app — everything platform-shaped. May use Qt freely.
# ─────────────────────────────────────────────────────────────────────────────

if(ANYTIME_BUILD_APP)

    set(ANYTIME_ADAPTER_HEADERS
        adapters/clock/QtClock.h
        adapters/codec/QtCborCodec.h
        adapters/events/FanOutEventSink.h
        adapters/notify/Notifier.h
        adapters/http/LocalHttpServer.h
        adapters/http/SseHub.h
        adapters/crypto/NoiseProtocol.h
        adapters/crypto/NoiseSessionFactory.h
        adapters/http/HttpTypes.h
        adapters/http/Router.h
        adapters/http/ApiRoutes.h
        adapters/http/JsonMapping.h
        adapters/identity/MonocypherIdentityStore.h
        adapters/logging/LogSetup.h
        adapters/support/SupportLinks.h
        adapters/persistence/settings/SettingsStore.h
        adapters/persistence/sqlite/SqliteStore.h
        adapters/persistence/sqlite/MonocypherDataCipher.h
        adapters/transport/loopback/LoopbackTransport.h
    )

    # These do not exist yet — write them, then flip ANYTIME_BUILD_APP on.
    set(ANYTIME_ADAPTER_SOURCES
        adapters/clock/QtClock.cpp
        adapters/codec/QtCborCodec.cpp
        adapters/crypto/NoiseProtocol.cpp
        adapters/crypto/NoiseSessionFactory.cpp
        adapters/http/HttpTypes.cpp
        adapters/http/Router.cpp
        adapters/http/LocalHttpServer.cpp
        adapters/http/SseHub.cpp
        adapters/http/ApiRoutes.cpp
        adapters/http/JsonMapping.cpp
        adapters/identity/MonocypherIdentityStore.cpp
        adapters/logging/LogSetup.cpp
        adapters/support/SupportLinks.cpp
        adapters/notify/Notifier.cpp
        adapters/persistence/settings/SettingsStore.cpp
        adapters/persistence/sqlite/SqliteStore.cpp
        adapters/persistence/sqlite/MonocypherDataCipher.cpp
        adapters/transport/loopback/LoopbackTransport.cpp
    )

    # ── BLE transport — Android only ────────────────────────────────────────
    # The JNI binding onto platform/android/src/com/anytimechat/ble/BleRadio.java.
    # Desktop has no radio by design (AD-07) and uses the loopback transport.
    if(ANDROID)
        list(APPEND ANYTIME_ADAPTER_HEADERS adapters/transport/ble/BleGattTransport.h)
        list(APPEND ANYTIME_ADAPTER_SOURCES adapters/transport/ble/BleGattTransport.cpp)

        # Turning the radio ON — IRadioController over BluetoothBridge.java.
        # Android-only for the same reason the radios are: ACTION_REQUEST_ENABLE
        # exists nowhere else, and desktop has no radio to switch (AD-07).
        list(APPEND ANYTIME_ADAPTER_HEADERS adapters/radio/AndroidRadioController.h)
        list(APPEND ANYTIME_ADAPTER_SOURCES adapters/radio/AndroidRadioController.cpp)

        # Message and request notifications — INotificationPresenter over
        # MessageNotifier.java. What to say is decided in Notifier.cpp, which
        # builds everywhere and is unit tested; only the showing is Android's.
        list(APPEND ANYTIME_ADAPTER_HEADERS adapters/notify/AndroidNotificationPresenter.h)
        list(APPEND ANYTIME_ADAPTER_SOURCES adapters/notify/AndroidNotificationPresenter.cpp)

        # Nearby Connections — the bulk transport for photos. Android-only, like
        # the BLE radio: it binds a Play Services API that exists nowhere else.
        # iOS gets MultipeerConnectivity as a third ITransport when that port
        # happens, which is deprioritised for v1.
        list(APPEND ANYTIME_ADAPTER_HEADERS
             adapters/transport/nearby/NearbyConnectionsTransport.h)
        list(APPEND ANYTIME_ADAPTER_SOURCES
             adapters/transport/nearby/NearbyConnectionsTransport.cpp)
    endif()

    add_library(anytime_adapters STATIC
        ${ANYTIME_ADAPTER_SOURCES}
        ${ANYTIME_ADAPTER_HEADERS}
    )
    # Qt6::Sql backs the repositories (section 12). Its QSQLITE driver is a
    # plugin, which androiddeployqt bundles because this target links the module
    # — that plugin step is the one wrinkle the architecture doc warns about.
    # Qt6::Gui is for QDesktopServices: the report route hands a mail draft to
    # the phone's mail app, and the links route opens our pages in the browser.
    target_link_libraries(anytime_adapters
        PUBLIC anytime_core Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql monocypher
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
            QT_ANDROID_PACKAGE_NAME "org.techyskitter.anytimechat.app")

        # versionCode must increase monotonically for Play. major*100 + minor
        # gives a sane ladder as long as minor stays under 100.
        math(EXPR ANYTIME_VERSION_CODE
             "${PROJECT_VERSION_MAJOR} * 100 + ${PROJECT_VERSION_MINOR}")
        set_property(TARGET anytime PROPERTY QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}")
        set_property(TARGET anytime PROPERTY QT_ANDROID_VERSION_CODE "${ANYTIME_VERSION_CODE}")

        # ── Stage the React bundle into the APK ──────────────────────────────
        #
        # The local HTTP server reads the UI from assets:/www at runtime, so the
        # built bundle has to be inside the APK. Copying it here rather than
        # committing it keeps one source of truth: ui/dist is generated, and a
        # stale checked-in copy is the kind of thing nobody notices until the
        # app ships with last week's screens.
        set(ANYTIME_ANDROID_WWW
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/assets/www")

        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist/index.html")
            message(WARNING
                "ui/dist/index.html is missing — the APK will contain no UI.
"
                "   Build it first:  cd ui && npm install && npm run build:android")
        endif()

        add_custom_target(copy_ui_assets
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${ANYTIME_ANDROID_WWW}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ANYTIME_ANDROID_WWW}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist" "${ANYTIME_ANDROID_WWW}"
            COMMENT "Staging ui/dist -> platform/android/assets/www"
            VERBATIM
        )
        add_dependencies(anytime copy_ui_assets)
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
message(STATUS "  log level .......... Debug: debug | other configs: ${ANYTIME_RELEASE_LOG_LEVEL}")
message(STATUS "  next up ............ see docs/SKELETON.md 'Build order'")
message(STATUS "")
