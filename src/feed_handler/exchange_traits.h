// What differs between exchanges in a connection config entry, as data.
//
// One ExchangeTraits per exchange, defined next to that exchange's other facts
// (kraken/kraken_endpoints.h, deribit/deribit_endpoints.h). The config validator
// reads them instead of branching on the exchange, so adding an exchange is one
// new row here and one enumerator, not an edit in every rule.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace feed_handler {

/// What an endpoint looks like, and so how it is validated.
enum class EndpointKind : std::uint8_t {
    /// A `ws://` or `wss://` URL.
    kWebSocketUrl,
    /// `host:port`.
    kHostPort,
};

struct ExchangeTraits {
    /// The value of a config entry's `exchange` key, and the journal header tag.
    std::string_view name;
    /// The one feed the backend subscribes to.
    std::string_view feed;
    /// The most symbols one connection may subscribe to.
    std::size_t max_symbols_per_connection;
    /// Whether `env = "testnet"` can ever connect.
    bool has_testnet;
    EndpointKind endpoint_kind;
    /// Where to connect when an entry gives no `endpoint`. Empty means there is
    /// none this project has actually connected to, so the config must say.
    std::string_view default_prod_endpoint;
    std::string_view default_testnet_endpoint;
};

}  // namespace feed_handler
