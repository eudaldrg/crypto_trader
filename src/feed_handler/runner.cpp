#include "feed_handler/runner.h"

#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <thread>

#include "feed_handler/logging.h"

namespace feed_handler {

namespace {

// A signal handler may only touch a volatile sig_atomic_t, so this cannot be
// wrapped in anything nicer.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_signals_received = 0;

constexpr int kExitAfterSecondSignal = 130;  // 128 + SIGINT, the shell convention.

extern "C" void HandleSignal(int /*signal*/) {
    if (g_signals_received != 0) {
        // Async-signal-safe, unlike exit(): no destructors, no flushing.
        _exit(kExitAfterSecondSignal);
    }
    g_signals_received = 1;
}

}  // namespace

void InstallSignalHandlers() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
}

bool ShutdownRequested() {
    return g_signals_received != 0;
}

RunResult Run(std::span<const std::unique_ptr<CaptureConnection>> connections,
              const RunOptions& options) {
    RunResult result;

    while (true) {
        // Fatal first: a latched failure is reported even if a stop was
        // requested in the same poll.
        const auto failed = std::ranges::find_if(
            connections, [](const std::unique_ptr<CaptureConnection>& c) { return c->Fatal(); });
        if (failed != connections.end()) {
            result.fatal_id = std::string((*failed)->Id());
            LogError("[" + result.fatal_id + "] capture failed, stopping every connection");
            break;
        }
        if (options.should_stop()) {
            LogInfo("shutdown requested");
            break;
        }
        std::this_thread::sleep_for(options.poll);
    }

    // Every request before any join: each connection winds down concurrently.
    for (const auto& connection : connections) {
        connection->RequestStop();
    }
    for (const auto& connection : connections) {
        connection->Join();
    }
    return result;
}

}  // namespace feed_handler
