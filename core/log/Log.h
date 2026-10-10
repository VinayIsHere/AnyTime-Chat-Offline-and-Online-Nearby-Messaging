// core/log/Log.h
//
// The one logging header the codebase includes. spdlog does the work; this
// file exists so nothing else names it — swapping the backend edits these
// macros and adapters/logging, and nothing else.
//
// WHY MACROS. Two things only a macro can do: capture file and line for free,
// and let SPDLOG_ACTIVE_LEVEL delete calls below the compiled level. In a
// Release build an AT_LOG_DEBUG line is not skipped at runtime — it does not
// exist in the binary. So debug logging can be generous without costing a
// shipped build anything.
//
// Levels come from CMakeLists.txt:
//   Debug    debug and above
//   Release  ANYTIME_RELEASE_LOG_LEVEL (default: critical)
//
// WHERE lines go — console, rotating file, logcat — is decided in
// adapters/logging/LogSetup.cpp. The core only says what happened.
//
// ─── PRIVACY RULES. These are not style preferences. ────────────────────────
//
//   NEVER log message bodies, greetings, or any decrypted payload.
//   NEVER log key material, shared secrets, or the local API token.
//   Peer ids, message ids, byte lengths, counts and error codes are fine.
//
// A debug build's log file can be pulled off a phone with adb. Chat text in it
// would defeat the encryption the rest of the app exists to provide.
//
// Usage:
//   AT_LOG_INFO("mesh", "session established with {}", peerId.toHex());
//
// The tag must be a string literal; it is concatenated at compile time.

#pragma once

#include <spdlog/spdlog.h>

#define AT_LOG_TRACE(tag, msg, ...)    SPDLOG_TRACE("[" tag "] " msg, ##__VA_ARGS__)
#define AT_LOG_DEBUG(tag, msg, ...)    SPDLOG_DEBUG("[" tag "] " msg, ##__VA_ARGS__)
#define AT_LOG_INFO(tag, msg, ...)     SPDLOG_INFO("[" tag "] " msg, ##__VA_ARGS__)
#define AT_LOG_WARN(tag, msg, ...)     SPDLOG_WARN("[" tag "] " msg, ##__VA_ARGS__)
#define AT_LOG_ERROR(tag, msg, ...)    SPDLOG_ERROR("[" tag "] " msg, ##__VA_ARGS__)
#define AT_LOG_CRITICAL(tag, msg, ...) SPDLOG_CRITICAL("[" tag "] " msg, ##__VA_ARGS__)
