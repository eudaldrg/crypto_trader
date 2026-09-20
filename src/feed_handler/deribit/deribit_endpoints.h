// Deribit facts the config validator and the FIX client must agree on, kept in
// one dependency-free header so neither can drift from the other.
#pragma once

#include <limits>
#include <string_view>

#include "feed_handler/exchange_traits.h"

namespace feed_handler::deribit {

/// The FIX testnet, plain TCP, no TLS (experiments/deribit_fix_probe.py). The
/// only Deribit endpoint this project has connected to: a production one has to
/// be given explicitly in the config.
inline constexpr std::string_view kTestnetEndpoint = "fix-test.deribit.com:9881";

/// The one feed the Deribit backend subscribes to (full book, bid and offer).
inline constexpr std::string_view kBookFeed = "book";

inline constexpr ExchangeTraits kTraits{
    .name = "deribit",
    .feed = kBookFeed,
    // No documented per-session cap on FIX NoRelatedSym is known here.
    .max_symbols_per_connection = std::numeric_limits<std::size_t>::max(),
    .has_testnet = true,
    .endpoint_kind = EndpointKind::kHostPort,
    .default_prod_endpoint = "",
    .default_testnet_endpoint = kTestnetEndpoint,
};

}  // namespace feed_handler::deribit
