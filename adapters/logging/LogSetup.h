// adapters/logging/LogSetup.h
//
// Chooses where log lines go. Called once, early, from app/main.cpp.
//
//   Desktop   coloured console + rotating file
//   Android   logcat (the phone's console) + rotating file
//
// The level is compiled in (see core/log/Log.h and CMakeLists.txt); this file
// only applies it at runtime and wires the sinks.

#pragma once

#include <QString>

#include <cstddef>
#include <string>

namespace anytime::logging {

struct LogConfig {
    /// Directory for anytime.log and its rotated siblings.
    QString directory;

    /// logcat tag on Android: filter with `adb logcat -s AnyTime`.
    QString tag = QStringLiteral("AnyTime");

    std::size_t maxFileBytes = 5 * 1024 * 1024;
    std::size_t maxFiles = 3;
};

/// Installs the default logger. Returns false when the log file could not be
/// opened — the console sink still works, so logging never takes the app down.
bool initialise(const LogConfig&);

/// Routes qDebug / qInfo / qWarning / qCritical / qFatal into the same logger,
/// so Qt's own warnings land in the file next to ours instead of vanishing.
void installQtMessageBridge();

/// Flushes and releases every sink. Call before exit.
void shutdown();

/// Absolute path of the active log file, or empty when there is none.
QString currentLogFile();

/// "debug", "critical", ... — the level this binary was compiled with.
std::string compiledLevelName();

}  // namespace anytime::logging
