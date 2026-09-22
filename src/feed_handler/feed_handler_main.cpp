// The capture binary: connect to every configured exchange, subscribe to the
// configured symbols, and journal every inbound wire message until SIGINT. With
// `order_books = true` in the config it also feeds every connection into live
// order books on one thread (book_adapter); no strategy, no order entry
// (decisions/0004).
//
// Which connections to open is the `--config` TOML file (feed_handler/config),
// optionally narrowed with `--exchange` and `--only`. Credentials are named
// there by environment variable and read from the environment by
// ResolveCredentials; they are never read from a file by this process, never
// logged and never journaled. This file names no exchange: everything
// exchange-specific is behind CaptureConnection.
//
// This is where the books are wired in, and not in the feed_handler library: that
// library must not depend on book_adapter (which depends on it), and this binary
// is the one place that links both. See book_adapter/book_wiring.h.
#include "book_adapter/book_service.h"
#include "book_adapter/book_wiring.h"
#include "feed_handler/capture_set.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/credentials.h"
#include "feed_handler/logging.h"
#include "feed_handler/runner.h"
#include "feed_handler/selection.h"

namespace {

// The exit-code contract a supervisor can rely on: a bad invocation or
// environment is not the same failure as a capture that died mid-run.
constexpr int kExitCleanShutdown = 0;
constexpr int kExitCaptureFailed = 1;
constexpr int kExitStartupError = 2;

}  // namespace

int main(int argc, char** argv) {
    const auto command_line = feed_handler::ParseCommandLine(argc, argv);
    if (!command_line) {
        feed_handler::LogError(command_line.error());
        return kExitStartupError;
    }
    const auto config = feed_handler::config::LoadConfigFile(command_line->config_path);
    if (!config) {
        feed_handler::LogError(config.error());
        return kExitStartupError;
    }
    const auto selected = feed_handler::SelectConnections(*config, *command_line);
    if (!selected) {
        feed_handler::LogError(selected.error());
        return kExitStartupError;
    }
    // Before anything starts: a capture asked to run books it cannot build is
    // refused, not run without them.
    if (const auto books_ok = book_adapter::CheckLiveBookConfig(*config, *selected); !books_ok) {
        feed_handler::LogError(books_ok.error());
        return kExitStartupError;
    }
    // Every missing variable is reported by NAME at once; values never are.
    auto resolved = feed_handler::ResolveCredentials(*selected, feed_handler::SystemEnv());
    if (!resolved) {
        feed_handler::LogError(resolved.error());
        return kExitStartupError;
    }

    // Declared BEFORE the set: the sessions inside it keep pointers to the books'
    // sinks, so the books have to be destroyed after the connections. With
    // `order_books` off no connection is added, and no thread is ever started.
    book_adapter::BookService books(book_adapter::BookService::Config{});

    auto set = feed_handler::CaptureSet::Build(*config, std::move(*resolved));
    if (!set) {
        feed_handler::LogError(set.error());
        return kExitStartupError;
    }

    feed_handler::InstallSignalHandlers();
    set->StartAll(book_adapter::LiveBooksHook(*config, books));
    // Every connection has its ring by now; the Kraken ones were only added after
    // the AssetPairs lookup, which is why this cannot start earlier.
    books.Start();
    const feed_handler::RunResult result = feed_handler::Run(set->Connections());

    // Run stopped and joined every connection, so nothing produces any more:
    // only now drain the rings and end the book thread. Books never change the
    // exit code below; a book problem is in its own summary lines.
    books.Stop();

    for (const auto& connection : set->Connections()) {
        feed_handler::LogInfo(connection->Summary());
    }
    books.LogSummaries();
    return result.Fatal() ? kExitCaptureFailed : kExitCleanShutdown;
}
