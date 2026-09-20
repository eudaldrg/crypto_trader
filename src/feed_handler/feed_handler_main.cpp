// The capture binary: connect to every configured exchange, subscribe to the
// configured symbols, and journal every inbound wire message until SIGINT. No
// order book, no strategy, no order entry (decisions/0004).
//
// Which connections to open is the `--config` TOML file (feed_handler/config),
// optionally narrowed with `--exchange` and `--only`. Credentials are named
// there by environment variable and read from the environment by
// ResolveCredentials; they are never read from a file by this process, never
// logged and never journaled. This file names no exchange: everything
// exchange-specific is behind CaptureConnection.
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
    // Every missing variable is reported by NAME at once; values never are.
    const auto credentials = feed_handler::ResolveCredentials(*selected, feed_handler::SystemEnv());
    if (!credentials) {
        feed_handler::LogError(credentials.error());
        return kExitStartupError;
    }

    auto set = feed_handler::CaptureSet::Build(*config, *selected, *credentials);
    if (!set) {
        feed_handler::LogError(set.error());
        return kExitStartupError;
    }

    feed_handler::InstallSignalHandlers();
    set->StartAll();
    const feed_handler::RunResult result = feed_handler::Run(set->Connections());

    for (const auto& connection : set->Connections()) {
        feed_handler::LogInfo(connection->Summary());
    }
    return result.Fatal() ? kExitCaptureFailed : kExitCleanShutdown;
}
