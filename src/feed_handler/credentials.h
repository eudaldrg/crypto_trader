// Reading the credentials a connection names, and nothing else.
//
// The config holds environment variable NAMES, never values. This is the one
// place those variables are read, and it is built so a value cannot leak: the
// only strings it ever formats are variable names and connection ids. The
// environment is injected, so tests exercise every path without touching the
// real one.
#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "feed_handler/config/feed_handler_config.h"

namespace feed_handler {

/// Reads one environment variable: its value, or nullopt when unset.
using EnvReader = std::function<std::optional<std::string>(std::string_view)>;

/// The process environment.
EnvReader SystemEnv();

/// One connection's credential pair, whatever the exchange calls them: an API
/// key and secret (Kraken), or a client id and client secret (Deribit).
struct Credential {
    std::string key = {};
    std::string secret = {};
};

/// A connection together with the credentials it named, so nothing downstream
/// has to keep two parallel lists in step.
struct ResolvedConnection {
    config::Connection connection = {};
    Credential credential = {};
};

/// One ResolvedConnection per connection, in order. A variable that is unset or empty
/// is missing. The error names EVERY missing variable, tagged with its
/// connection id, so one run shows the whole list; it never contains a value,
/// including the values of the variables that were set. An entry is never
/// skipped for lack of credentials: that would look healthy while capturing
/// nothing.
std::expected<std::vector<ResolvedConnection>, std::string> ResolveCredentials(
    std::span<const config::Connection> connections, const EnvReader& env);

}  // namespace feed_handler
