#include "feed_handler/selection.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace feed_handler {

namespace {

using Error = std::unexpected<std::string>;

std::string Usage(std::string_view program) {
    return "usage: " + std::string(program) + " --config <path> [--exchange " +
           config::ExchangeNames("|") + "] [--only <id>[,<id>...]]";
}

/// Splits "a,b,c" into ids. An empty piece ("a,,b", a trailing comma) is an
/// error rather than being skipped: it is almost certainly a typo.
std::expected<std::vector<std::string>, std::string> SplitIds(std::string_view list) {
    std::vector<std::string> ids;
    while (true) {
        const std::size_t comma = list.find(',');
        const std::string_view id = list.substr(0, comma);
        if (id.empty()) {
            return Error("empty connection id in --only list");
        }
        ids.emplace_back(id);
        if (comma == std::string_view::npos) {
            return ids;
        }
        list.remove_prefix(comma + 1);
    }
}

std::string Describe(const CommandLine& command_line) {
    std::string filters;
    if (command_line.exchange) {
        filters += "--exchange " + std::string(config::ToString(*command_line.exchange));
    }
    for (const std::string& id : command_line.only_ids) {
        filters += filters.empty() ? "" : ", ";
        filters += "--only " + id;
    }
    return filters;
}

}  // namespace

std::expected<CommandLine, std::string> ParseCommandLine(int argc, const char* const* argv) {
    const std::span<const char* const> args(argv, static_cast<std::size_t>(argc));
    const std::string usage = Usage(args.empty() ? "feed_handler" : args[0]);
    const auto fail = [&usage](const std::string& reason) { return Error(reason + "\n" + usage); };

    CommandLine command_line;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        std::string_view name = arg;
        std::optional<std::string_view> value;
        if (const std::size_t equals = arg.find('='); arg.starts_with("--") && equals != arg.npos) {
            name = arg.substr(0, equals);
            value = arg.substr(equals + 1);
        }
        if (name != "--config" && name != "--only" && name != "--exchange") {
            return fail("unknown argument '" + std::string(arg) + "'");
        }
        if (!value) {
            if (index + 1 >= args.size()) {
                return fail("missing value for " + std::string(name));
            }
            value = args[++index];
        }
        if (value->empty()) {
            return fail("empty value for " + std::string(name));
        }

        if (name == "--config") {
            // Empty values are rejected above, so a non-empty path means it was
            // already given.
            if (!command_line.config_path.empty()) {
                return fail("--config given more than once");
            }
            command_line.config_path = std::string(*value);
        } else if (name == "--only") {
            auto ids = SplitIds(*value);
            if (!ids) {
                return fail(ids.error());
            }
            command_line.only_ids.insert(command_line.only_ids.end(), ids->begin(), ids->end());
        } else {
            if (command_line.exchange) {
                return fail("--exchange given more than once");
            }
            command_line.exchange = config::ExchangeFromString(*value);
            if (!command_line.exchange) {
                return fail("unknown exchange '" + std::string(*value) + "' (expected " +
                            config::ExchangeNames("|") + ")");
            }
        }
    }
    if (command_line.config_path.empty()) {
        return fail("--config is required");
    }
    return command_line;
}

std::expected<std::vector<config::Connection>, std::string> SelectConnections(
    const config::FeedHandlerConfig& config, const CommandLine& command_line) {
    for (const std::string& id : command_line.only_ids) {
        const bool known = std::ranges::any_of(
            config.connections, [&id](const config::Connection& c) { return c.id == id; });
        if (!known) {
            std::string configured;
            for (const config::Connection& connection : config.connections) {
                configured += configured.empty() ? "" : ", ";
                configured += connection.id;
            }
            return Error("--only names unknown connection id '" + id +
                         "' (configured: " + configured + ")");
        }
    }

    std::vector<config::Connection> selected;
    for (const config::Connection& connection : config.connections) {
        const bool id_ok =
            command_line.only_ids.empty() ||
            std::ranges::find(command_line.only_ids, connection.id) != command_line.only_ids.end();
        const bool exchange_ok =
            !command_line.exchange || connection.exchange == *command_line.exchange;
        if (id_ok && exchange_ok) {
            selected.push_back(connection);
        }
    }
    if (selected.empty()) {
        const std::string filters = Describe(command_line);
        return Error("no configured connection matches " +
                     (filters.empty() ? std::string("the selection") : "(" + filters + ")"));
    }
    return selected;
}

}  // namespace feed_handler
