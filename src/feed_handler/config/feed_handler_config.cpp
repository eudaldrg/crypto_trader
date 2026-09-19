#include "feed_handler/config/feed_handler_config.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <toml++/toml.hpp>
#include <utility>

namespace feed_handler::config {

namespace {

constexpr std::size_t kMaxIdLength = 48;
constexpr std::size_t kMaxSymbolLength = 64;
constexpr unsigned kMaxPort = 65'535;

// The only places a default endpoint exists are the two this project has
// actually connected to (exchanges/kraken.md, exchanges/deribit.md). Anything
// else must say where to connect rather than inherit a guessed hostname.
constexpr std::string_view kKrakenProdEndpoint = "wss://ws-l3.kraken.com/v2";
constexpr std::string_view kDeribitTestnetEndpoint = "fix-test.deribit.com:9881";

constexpr std::string_view kKrakenFeed = "level3";
constexpr std::string_view kDeribitFeed = "book";

using Error = std::unexpected<std::string>;

bool IsAsciiAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/// Lowercase letters, digits, `_` and `-`, starting with a letter or digit: the
/// id becomes a file name prefix, so no dots, slashes or case surprises.
bool IsValidId(std::string_view id) {
    if (id.empty() || id.size() > kMaxIdLength) {
        return false;
    }
    const auto is_lower_alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    return is_lower_alnum(id.front()) && std::ranges::all_of(id, [&](char c) {
               return is_lower_alnum(c) || c == '_' || c == '-';
           });
}

/// What Kraken ("BTC/USD") and Deribit ("BTC-27MAR26-100000-C") symbols are made
/// of. Deliberately narrow: symbols are spliced into a JSON string and into a
/// SOH-delimited FIX field without escaping, so a quote, a backslash or a
/// control byte in one would corrupt or inject into the message.
bool IsValidSymbol(std::string_view symbol) {
    if (symbol.empty() || symbol.size() > kMaxSymbolLength) {
        return false;
    }
    return std::ranges::all_of(symbol, [](char c) {
        return IsAsciiAlnum(c) || c == '_' || c == '-' || c == '.' || c == '/';
    });
}

/// A POSIX-portable environment variable name.
bool IsValidEnvName(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) { return IsAsciiAlnum(c) || c == '_'; });
}

bool IsValidHostPort(std::string_view endpoint) {
    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == endpoint.size()) {
        return false;
    }
    const std::string_view host = endpoint.substr(0, colon);
    const std::string_view port = endpoint.substr(colon + 1);
    if (!std::ranges::all_of(host,
                             [](char c) { return IsAsciiAlnum(c) || c == '-' || c == '.'; })) {
        return false;
    }
    if (!std::ranges::all_of(port, [](char c) { return c >= '0' && c <= '9'; }) ||
        port.size() > 5) {
        return false;
    }
    const unsigned value = static_cast<unsigned>(std::stoul(std::string(port)));
    return value >= 1 && value <= kMaxPort;
}

bool IsValidWebSocketUrl(std::string_view url) {
    const bool scheme_ok = url.starts_with("wss://") || url.starts_with("ws://");
    return scheme_ok && url.find_first_of(" \t\r\n\"\\") == std::string_view::npos &&
           url.size() > std::string_view("wss://").size();
}

std::string Label(std::size_t index, std::string_view id) {
    std::string label = "connections[" + std::to_string(index) + "]";
    if (!id.empty()) {
        label += " (" + std::string(id) + ")";
    }
    return label;
}

std::expected<void, std::string> RejectUnknownKeys(const toml::table& table,
                                                   std::initializer_list<std::string_view> allowed,
                                                   const std::string& where) {
    for (const auto& [key, value] : table) {
        if (std::ranges::find(allowed, key.str()) == allowed.end()) {
            return Error(where + ": unknown key '" + std::string(key.str()) + "'");
        }
    }
    return {};
}

std::expected<std::optional<std::string>, std::string> ReadString(const toml::table& table,
                                                                  std::string_view key,
                                                                  const std::string& where) {
    const toml::node* node = table.get(key);
    if (node == nullptr) {
        return std::nullopt;
    }
    if (!node->is_string()) {
        return Error(where + ": '" + std::string(key) + "' must be a string");
    }
    return node->as_string()->get();
}

std::expected<std::string, std::string> RequireString(const toml::table& table,
                                                      std::string_view key,
                                                      const std::string& where) {
    auto value = ReadString(table, key, where);
    if (!value) {
        return Error(value.error());
    }
    if (!*value) {
        return Error(where + ": missing required key '" + std::string(key) + "'");
    }
    return std::move(**value);
}

std::expected<Exchange, std::string> ParseExchange(std::string_view text,
                                                   const std::string& where) {
    if (text == "kraken") {
        return Exchange::kKraken;
    }
    if (text == "deribit") {
        return Exchange::kDeribit;
    }
    return Error(where + ": unknown exchange '" + std::string(text) +
                 "' (expected \"kraken\" or \"deribit\")");
}

std::expected<Environment, std::string> ParseEnvironment(std::string_view text,
                                                         const std::string& where) {
    if (text == "prod") {
        return Environment::kProd;
    }
    if (text == "testnet") {
        return Environment::kTestnet;
    }
    return Error(where + ": unknown env '" + std::string(text) +
                 "' (expected \"prod\" or \"testnet\")");
}

std::expected<std::vector<std::string>, std::string> ParseSymbols(const toml::table& table,
                                                                  Exchange exchange,
                                                                  const std::string& where) {
    const toml::node* node = table.get("symbols");
    if (node == nullptr) {
        return Error(where + ": missing required key 'symbols'");
    }
    const toml::array* array = node->as_array();
    if (array == nullptr) {
        return Error(where + ": 'symbols' must be an array of strings");
    }
    if (array->empty()) {
        return Error(where + ": 'symbols' must not be empty");
    }
    if (exchange == Exchange::kKraken && array->size() > kKrakenMaxSymbolsPerConnection) {
        return Error(where + ": " + std::to_string(array->size()) +
                     " symbols exceeds Kraken's limit of " +
                     std::to_string(kKrakenMaxSymbolsPerConnection) +
                     " per connection; split them across several [[connections]]");
    }

    std::vector<std::string> symbols;
    std::set<std::string> seen;
    for (const toml::node& element : *array) {
        const toml::value<std::string>* text = element.as_string();
        if (text == nullptr) {
            return Error(where + ": 'symbols' must contain only strings");
        }
        if (!IsValidSymbol(text->get())) {
            return Error(where + ": invalid symbol '" + text->get() +
                         "' (letters, digits and _ - . / only, at most " +
                         std::to_string(kMaxSymbolLength) + " characters)");
        }
        if (!seen.insert(text->get()).second) {
            return Error(where + ": duplicate symbol '" + text->get() + "'");
        }
        symbols.push_back(text->get());
    }
    return symbols;
}

std::expected<std::string, std::string> ResolveEndpoint(const toml::table& table, Exchange exchange,
                                                        Environment env, const std::string& where) {
    auto given = ReadString(table, "endpoint", where);
    if (!given) {
        return Error(given.error());
    }
    if (!*given) {
        if (exchange == Exchange::kKraken && env == Environment::kProd) {
            return std::string(kKrakenProdEndpoint);
        }
        if (exchange == Exchange::kDeribit && env == Environment::kTestnet) {
            return std::string(kDeribitTestnetEndpoint);
        }
        return Error(where + ": no default endpoint for " + std::string(ToString(exchange)) + " " +
                     std::string(ToString(env)) + "; set 'endpoint'");
    }
    const bool valid =
        exchange == Exchange::kKraken ? IsValidWebSocketUrl(**given) : IsValidHostPort(**given);
    if (!valid) {
        return Error(where + ": invalid endpoint '" + **given + "' (" +
                     (exchange == Exchange::kKraken ? "expected a ws:// or wss:// URL"
                                                    : "expected host:port") +
                     ")");
    }
    return std::move(**given);
}

std::expected<std::string, std::string> RequireEnvName(const toml::table& table,
                                                       std::string_view key,
                                                       const std::string& where) {
    auto name = RequireString(table, key, where);
    if (!name) {
        return name;
    }
    if (!IsValidEnvName(*name)) {
        // The value is an environment variable *name*, so it is safe to echo;
        // it is what someone would get wrong by pasting the secret itself.
        return Error(where + ": '" + std::string(key) +
                     "' must be the NAME of an environment variable, never the credential itself");
    }
    return name;
}

std::expected<Connection, std::string> ParseConnection(const toml::table& table,
                                                       std::size_t index) {
    std::string where = Label(index, {});
    if (auto ok = RejectUnknownKeys(table,
                                    {"id", "exchange", "env", "feed", "symbols", "endpoint",
                                     "api_key_env", "api_secret_env"},
                                    where);
        !ok) {
        return Error(ok.error());
    }

    Connection connection;
    auto id = RequireString(table, "id", where);
    if (!id) {
        return Error(id.error());
    }
    connection.id = std::move(*id);
    where = Label(index, connection.id);
    if (!IsValidId(connection.id)) {
        return Error(where + ": 'id' must be lowercase letters, digits, _ and - (at most " +
                     std::to_string(kMaxIdLength) + " characters); it names the journal files");
    }

    auto exchange_text = RequireString(table, "exchange", where);
    if (!exchange_text) {
        return Error(exchange_text.error());
    }
    auto exchange = ParseExchange(*exchange_text, where);
    if (!exchange) {
        return Error(exchange.error());
    }
    connection.exchange = *exchange;

    auto env_text = RequireString(table, "env", where);
    if (!env_text) {
        return Error(env_text.error());
    }
    auto env = ParseEnvironment(*env_text, where);
    if (!env) {
        return Error(env.error());
    }
    connection.env = *env;

    const std::string_view supported_feed =
        connection.exchange == Exchange::kKraken ? kKrakenFeed : kDeribitFeed;
    auto feed = ReadString(table, "feed", where);
    if (!feed) {
        return Error(feed.error());
    }
    if (feed->has_value() && **feed != supported_feed) {
        return Error(where + ": unsupported feed '" + **feed + "' for " +
                     std::string(ToString(connection.exchange)) + " (only \"" +
                     std::string(supported_feed) + "\")");
    }
    connection.feed = std::string(supported_feed);

    auto symbols = ParseSymbols(table, connection.exchange, where);
    if (!symbols) {
        return Error(symbols.error());
    }
    connection.symbols = std::move(*symbols);

    auto endpoint = ResolveEndpoint(table, connection.exchange, connection.env, where);
    if (!endpoint) {
        return Error(endpoint.error());
    }
    connection.endpoint = std::move(*endpoint);

    auto api_key_env = RequireEnvName(table, "api_key_env", where);
    if (!api_key_env) {
        return Error(api_key_env.error());
    }
    auto api_secret_env = RequireEnvName(table, "api_secret_env", where);
    if (!api_secret_env) {
        return Error(api_secret_env.error());
    }
    if (*api_key_env == *api_secret_env) {
        return Error(where + ": 'api_key_env' and 'api_secret_env' name the same variable");
    }
    connection.api_key_env = std::move(*api_key_env);
    connection.api_secret_env = std::move(*api_secret_env);
    return connection;
}

std::expected<std::filesystem::path, std::string> ReadPath(const toml::table& table,
                                                           std::string_view key,
                                                           std::filesystem::path fallback) {
    auto text = ReadString(table, key, "config");
    if (!text) {
        return Error(text.error());
    }
    if (!*text) {
        return fallback;
    }
    if ((*text)->empty()) {
        return Error("config: '" + std::string(key) + "' must not be empty");
    }
    return std::filesystem::path(**text);
}

}  // namespace

std::string_view ToString(Exchange exchange) {
    switch (exchange) {
        case Exchange::kKraken:
            return "kraken";
        case Exchange::kDeribit:
            return "deribit";
    }
    return "unknown";
}

std::string_view ToString(Environment env) {
    switch (env) {
        case Environment::kProd:
            return "prod";
        case Environment::kTestnet:
            return "testnet";
    }
    return "unknown";
}

std::vector<Connection> FeedHandlerConfig::ConnectionsFor(Exchange exchange) const {
    std::vector<Connection> result;
    std::ranges::copy_if(connections, std::back_inserter(result),
                         [exchange](const Connection& c) { return c.exchange == exchange; });
    return result;
}

std::expected<FeedHandlerConfig, std::string> ParseConfig(std::string_view toml_text,
                                                          std::string_view source_name) {
    toml::table root;
    try {
        root = toml::parse(toml_text, source_name);
    } catch (const toml::parse_error& error) {
        return Error(std::string(source_name) + ":" + std::to_string(error.source().begin.line) +
                     ":" + std::to_string(error.source().begin.column) + ": " +
                     std::string(error.description()));
    }

    if (auto ok = RejectUnknownKeys(root, {"journal_dir", "state_dir", "connections"}, "config");
        !ok) {
        return Error(ok.error());
    }

    FeedHandlerConfig config;
    auto journal_dir = ReadPath(root, "journal_dir", config.journal_dir);
    if (!journal_dir) {
        return Error(journal_dir.error());
    }
    config.journal_dir = std::move(*journal_dir);
    auto state_dir = ReadPath(root, "state_dir", config.state_dir);
    if (!state_dir) {
        return Error(state_dir.error());
    }
    config.state_dir = std::move(*state_dir);

    const toml::node* node = root.get("connections");
    if (node == nullptr) {
        return Error("config: missing required [[connections]] table");
    }
    const toml::array* array = node->as_array();
    if (array != nullptr && array->empty()) {
        // Before the tables check: toml++ says an empty array is not an array of
        // tables, which would report the wrong problem.
        return Error("config: at least one [[connections]] entry is required");
    }
    if (array == nullptr || !array->is_array_of_tables()) {
        return Error("config: 'connections' must be an array of tables ([[connections]])");
    }

    std::set<std::string> ids;
    std::size_t index = 0;
    for (const toml::node& element : *array) {
        auto connection = ParseConnection(*element.as_table(), index);
        if (!connection) {
            return Error(connection.error());
        }
        if (!ids.insert(connection->id).second) {
            return Error(Label(index, connection->id) + ": duplicate connection id");
        }
        config.connections.push_back(std::move(*connection));
        ++index;
    }
    return config;
}

std::expected<FeedHandlerConfig, std::string> LoadConfigFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return Error("cannot open config file " + path.string());
    }
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) {
        return Error("cannot read config file " + path.string());
    }
    return ParseConfig(text.str(), path.string());
}

std::expected<std::filesystem::path, std::string> ConfigPathFromArgs(int argc,
                                                                     const char* const* argv) {
    const std::span<const char* const> args(argv, static_cast<std::size_t>(argc));
    const std::string usage =
        "usage: " + std::string(args.empty() ? "feed_handler" : args[0]) + " --config <path>";

    constexpr std::string_view kFlag = "--config";
    std::optional<std::filesystem::path> path;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        if (arg == kFlag && index + 1 < args.size() && !path) {
            path = args[++index];
        } else if (arg.starts_with("--config=") && arg.size() > kFlag.size() + 1 && !path) {
            path = std::string(arg.substr(kFlag.size() + 1));
        } else {
            return Error(usage);
        }
    }
    if (!path) {
        return Error(usage);
    }
    return *path;
}

std::string EnvOrEmpty(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    return value == nullptr ? std::string{} : std::string(value);
}

}  // namespace feed_handler::config
