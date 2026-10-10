// tests/LoggingTests.cpp
//
// One property, because its absence crashed the app on every exit: logging must
// still work after logging::shutdown().
//
// main() shuts logging down and THEN returns, which destroys its locals — the
// HTTP server among them, whose open SSE socket logs as it closes. When
// shutdown() dropped the default logger that line dereferenced null, and both
// test handsets recorded a native crash each time the app was closed.

#include "tests/TestHarness.h"

#include "adapters/logging/LogSetup.h"
#include "core/log/Log.h"

namespace anytime::test {

void runLoggingTests(Registry& registry)
{
    registry.section("Logging — still usable after shutdown");

    // Console only: no directory, so nothing is written to disk. The flush
    // keeps initialise()'s own start-up line from landing mid-way through the
    // report, which is still buffered at this point.
    std::fflush(stdout);
    logging::initialise(logging::LogConfig{});
    spdlog::set_level(spdlog::level::off);  // keep the report readable

    logging::shutdown();

    registry.check(spdlog::default_logger_raw() != nullptr,
                   "the default logger survives shutdown");

    // The line that used to crash, in the shape SseHub logs it.
    const std::size_t open = 0;
    AT_LOG_DEBUG("sse", "event stream closed ({} open)", open);
    registry.check(true, "a line logged after shutdown does not crash");

    logging::shutdown();
    registry.check(spdlog::default_logger_raw() != nullptr,
                   "shutting down twice is harmless");
}

}  // namespace anytime::test
