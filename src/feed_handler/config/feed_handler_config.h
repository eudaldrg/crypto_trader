// TOML configuration for the feed-handler binaries: which connections to open,
// to which symbols, with which credentials, journaling where.
//
// One `[[connections]]` entry is one socket and one journal file, which is what
// turns "capture a second instrument" or "shard past Kraken's 200-symbol cap"
// into a config edit. The schema and its rationale are in
// docs/modules/feed-handler.md.
//
// Validation is strict and up front: an unknown key, a wrong type, a duplicate
// id or a symbol that could not survive being spliced into a wire message is a
// startup error with a message naming the entry, never a silent default. That
// matters more than it looks, because these strings are interpolated into
// hand-built JSON and FIX messages (kraken_ws_client.h, deribit_fix_session.h)
// and a typo such as `symbol = ...` for `symbols = ...` would otherwise
// capture nothing while looking healthy.
//
// Credentials are never in the file: `api_key_env`/`api_secret_env` name the
// environment variables that hold them. This module never reads those;
// feed_handler/credentials.h does, and it is the only code that does.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feed_handler::config {

enum class Exchange : std::uint8_t {
    kKraken,
    kDeribit,
};

enum class Environment : std::uint8_t {
    kProd,
    kTestnet,
};

std::string_view ToString(Exchange exchange);
std::string_view ToString(Environment env);

/// Every exchange, the one list the config parser, the command line and their
/// usage text are built from. A new Exchange enumerator goes here and in
/// ToString (whose switch the compiler checks).
inline constexpr std::array<Exchange, 2> kAllExchanges = {Exchange::kKraken, Exchange::kDeribit};

/// The exchange whose ToString() is `name`, or nullopt.
std::optional<Exchange> ExchangeFromString(std::string_view name);

/// Every exchange's name joined by `separator`: "kraken|deribit".
std::string ExchangeNames(std::string_view separator);

struct HostPort {
    std::string host;
    std::uint16_t port = 0;
};

/// One connection, fully resolved: every default has been applied, so a
/// consumer never needs to know which keys the file actually spelled out.
struct Connection {
    /// Connection identity and journal file name prefix.
    std::string id;
    Exchange exchange = Exchange::kKraken;
    Environment env = Environment::kProd;
    /// "level3" for Kraken, "book" for Deribit: the only feed each supports so
    /// far, kept as a key so a second one is not a schema change.
    std::string feed;
    std::vector<std::string> symbols;
    /// Kraken: a `ws://` or `wss://` URL. Deribit: `host:port`.
    std::string endpoint;
    /// Deribit only: `endpoint` already split and validated, so nothing has to
    /// parse it again. Empty host and port 0 for Kraken.
    HostPort host_port = {};
    /// Names of the environment variables holding the credentials, never the
    /// credentials themselves.
    std::string api_key_env;
    std::string api_secret_env;
};

struct FeedHandlerConfig {
    /// Both are resolved against the process's working directory when relative.
    std::filesystem::path journal_dir = "journal";
    /// Local runtime state that belongs to this machine (Kraken's nonce mark).
    std::filesystem::path state_dir = "state";
    std::vector<Connection> connections;
};

/// Parses and validates TOML text. `source_name` only labels parse errors.
std::expected<FeedHandlerConfig, std::string> ParseConfig(std::string_view toml_text,
                                                          std::string_view source_name = "config");

/// Reads and parses a file; the error names the path.
std::expected<FeedHandlerConfig, std::string> LoadConfigFile(const std::filesystem::path& path);

/// Splits and validates a Deribit-style `host:port` endpoint: a hostname of
/// letters, digits, `-` and `.`, and a port in 1..65535.
std::expected<HostPort, std::string> ParseHostPort(std::string_view endpoint);

}  // namespace feed_handler::config
