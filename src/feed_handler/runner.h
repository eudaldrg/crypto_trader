// Running a set of captures until asked to stop or one fails.
//
// Split out of main() so the parts that matter -- what a fatal connection does
// to the others, and the order things are torn down in -- are testable with
// fake connections instead of live sockets.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "feed_handler/capture_connection.h"

namespace feed_handler {

struct RunOptions {
    /// How often to look for a stop request or a fatal connection.
    std::chrono::milliseconds poll = std::chrono::milliseconds{100};
    /// Asked once per poll; true ends the run cleanly. Empty means "until a
    /// SIGINT/SIGTERM arrives" (ShutdownRequested()).
    std::function<bool()> should_stop = nullptr;
};

struct RunResult {
    /// A connection went fatal (a journal could not be opened or written).
    bool fatal = false;
    /// Which one, when `fatal`; the first found in the order given.
    std::string fatal_id = {};
};

/// Polls until `options.should_stop()` (or a signal) or any connection's
/// Fatal(), then stops EVERY connection: RequestStop on all of them first and
/// only then Join on all of them, once each. Requesting before joining is what
/// makes shutdown of N connections cost one wind-down instead of N. Does not
/// Start anything; the caller decides the start order.
///
/// A fatal connection stops all of them, deliberately and with no option: the
/// common cause is a shared disk, and the log names which connection latched.
/// A fatal that is already latched is reported even when a stop was requested
/// at the same time.
RunResult Run(std::span<const std::unique_ptr<CaptureConnection>> connections,
              const RunOptions& options = {});

/// Installs SIGINT and SIGTERM handlers. The first signal makes
/// ShutdownRequested() true. A SECOND signal calls _exit(130): shutdown of
/// several connections takes longer than a single one did, and a second Ctrl-C
/// must not be swallowed while it drags on.
void InstallSignalHandlers();

/// True once a SIGINT or SIGTERM has arrived.
bool ShutdownRequested();

}  // namespace feed_handler
