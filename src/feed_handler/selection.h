// The feed handler's command line and which configured connections it selects.
//
// Kept apart from main() so both are unit-testable without a process. The
// selection is deliberately strict: an id that names nothing, or a filter that
// leaves no connection, is an error, because the alternative is a process that
// starts, looks healthy and captures nothing.
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "feed_handler/config/feed_handler_config.h"

namespace feed_handler {

struct CommandLine {
    /// `--config <path>`, required.
    std::filesystem::path config_path = {};
    /// `--only <id>[,<id>...]`, repeatable. Empty means every connection.
    std::vector<std::string> only_ids = {};
    /// `--exchange kraken|deribit`. Empty means every exchange.
    std::optional<config::Exchange> exchange = std::nullopt;
};

/// Parses `--config`, `--only` and `--exchange`, each as `--flag value` or
/// `--flag=value`. `--config` and `--exchange` may be given once, `--only` any
/// number of times. Anything else is an error, and every error carries the
/// usage line.
std::expected<CommandLine, std::string> ParseCommandLine(int argc, const char* const* argv);

/// The configured connections that satisfy every filter in `command_line`, in
/// file order. `--exchange` and `--only` narrow together. An unknown id and an
/// empty result are both errors.
std::expected<std::vector<config::Connection>, std::string> SelectConnections(
    const config::FeedHandlerConfig& config, const CommandLine& command_line);

}  // namespace feed_handler
