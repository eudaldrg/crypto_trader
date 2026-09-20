// Kraken facts the config validator and the WebSocket client must agree on,
// kept in one dependency-free header so neither can drift from the other.
#pragma once

#include <cstddef>
#include <string_view>

#include "feed_handler/exchange_traits.h"

namespace feed_handler::kraken {

/// The production level3 endpoint (exchanges/kraken.md). The only one this
/// project has connected to; there is no Kraken testnet for level3.
inline constexpr std::string_view kDefaultWsUrl = "wss://ws-l3.kraken.com/v2";

/// Kraken refuses more than this many symbols on one WebSocket connection
/// (docs.kraken.com; the capture-scope notes in decisions/0004).
inline constexpr std::size_t kMaxSymbolsPerConnection = 200;

/// The one feed the Kraken backend subscribes to.
inline constexpr std::string_view kLevel3Feed = "level3";

inline constexpr ExchangeTraits kTraits{
    .name = "kraken",
    .feed = kLevel3Feed,
    .max_symbols_per_connection = kMaxSymbolsPerConnection,
    .has_testnet = false,
    .endpoint_kind = EndpointKind::kWebSocketUrl,
    .default_prod_endpoint = kDefaultWsUrl,
    .default_testnet_endpoint = "",
};

}  // namespace feed_handler::kraken
