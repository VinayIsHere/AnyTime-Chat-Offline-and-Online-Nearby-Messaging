// adapters/logging/LogSetup.cpp

#include "adapters/logging/LogSetup.h"

#include <QDir>
#include <QtGlobal>

#include <memory>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>

#ifdef Q_OS_ANDROID
#include <spdlog/sinks/android_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#include "core/log/Log.h"

namespace anytime::logging {

namespace {

QString g_logFile;

spdlog::level::level_enum compiledLevel()
{
    return static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL);
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    spdlog::level::level_enum level = spdlog::level::info;
    switch (type) {
        case QtDebugMsg:    level = spdlog::level::debug;    break;
        case QtInfoMsg:     level = spdlog::level::info;     break;
        case QtWarningMsg:  level = spdlog::level::warn;     break;
        case QtCriticalMsg: level = spdlog::level::err;      break;
        case QtFatalMsg:    level = spdlog::level::critical; break;
    }

    spdlog::logger* logger = spdlog::default_logger_raw();
    if (logger == nullptr) return;

    if (logger->should_log(level)) {
        // Qt strips file/line from release builds; an empty source_loc then
        // prints nothing rather than a null filename.
        const spdlog::source_loc where =
            (context.file != nullptr && context.line > 0)
                ? spdlog::source_loc{context.file, context.line,
                                     context.function != nullptr ? context.function : ""}
                : spdlog::source_loc{};
        logger->log(where, level, "[qt] {}", message.toStdString());
    }

    // Qt aborts the process as soon as this handler returns for a fatal
    // message. Whatever is buffered at that moment is the most useful thing in
    // the file, so flush before letting it happen.
    if (type == QtFatalMsg) logger->flush();
}

}  // namespace

bool initialise(const LogConfig& config)
{
    std::vector<spdlog::sink_ptr> sinks;

#ifdef Q_OS_ANDROID
    // stdout goes nowhere on Android; logcat is the phone's console.
    sinks.push_back(std::make_shared<spdlog::sinks::android_sink_mt>(config.tag.toStdString()));
#else
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif

    bool fileOpened = false;
    std::string fileError;

    if (!config.directory.isEmpty()) {
        QDir().mkpath(config.directory);
        g_logFile = QDir(config.directory).filePath(QStringLiteral("anytime.log"));

        // spdlog opens files with narrow APIs; on Windows that means the local
        // code page rather than UTF-8.
#ifdef Q_OS_WIN
        const std::string nativePath = QDir::toNativeSeparators(g_logFile).toLocal8Bit().toStdString();
#else
        const std::string nativePath = g_logFile.toStdString();
#endif
        try {
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                nativePath, config.maxFileBytes, config.maxFiles));
            fileOpened = true;
        } catch (const spdlog::spdlog_ex& e) {
            fileError = e.what();
            g_logFile.clear();
        }
    }

    auto logger = std::make_shared<spdlog::logger>("anytime", sinks.begin(), sinks.end());
    logger->set_level(compiledLevel());

    // Flush every line this build keeps. A crash is exactly when the last lines
    // matter, and buffered ones die with the process. In a Release build that
    // keeps only critical lines this costs nothing.
    logger->flush_on(compiledLevel());

    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v  (%s:%#)");

    spdlog::set_default_logger(logger);

    if (!fileOpened && !config.directory.isEmpty()) {
        AT_LOG_WARN("log", "could not open log file, console only: {}", fileError);
    }
    AT_LOG_INFO("log", "logging at level '{}' to {}", compiledLevelName(),
                g_logFile.isEmpty() ? std::string("console only") : g_logFile.toStdString());

    return fileOpened;
}

void installQtMessageBridge()
{
    qInstallMessageHandler(qtMessageHandler);
}

void shutdown()
{
    // FLUSH, DO NOT DESTROY.
    //
    // This used to call spdlog::shutdown(), which drops the default logger and
    // leaves default_logger_raw() null. main() called it before returning, and
    // its locals are destroyed AFTER that: ~LocalHttpServer deletes the open SSE
    // socket, the socket emits disconnected(), SseHub logs "event stream
    // closed" — through a null logger. Every exit of the app with the page open
    // was a SIGSEGV at 0x38 inside spdlog::logger::should_log, on both test
    // handsets.
    //
    // Nothing here needs destroying. The loggers are synchronous, so there is no
    // worker thread to join, and the registry releases the sinks itself at
    // process exit. Keeping the logger alive means a destructor, a late Qt
    // message or a radio callback on another thread can still log safely.
    spdlog::logger* logger = spdlog::default_logger_raw();
    if (logger == nullptr) return;

    AT_LOG_INFO("log", "shutting down");
    logger->flush();
}

QString currentLogFile()
{
    return g_logFile;
}

std::string compiledLevelName()
{
    const auto name = spdlog::level::to_string_view(compiledLevel());
    return std::string(name.data(), name.size());
}

}  // namespace anytime::logging
