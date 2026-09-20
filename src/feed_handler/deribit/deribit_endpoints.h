// Deribit facts the config validator and the FIX client must agree on, kept in
// one dependency-free header so neither can drift from the other.
#pragma once

#include <cstdint>
#include <string_view>

namespace feed_handler::deribit {

/// The FIX testnet, plain TCP, no TLS (experiments/deribit_fix_probe.py). The
/// only Deribit endpoint this project has connected to: a production one has
/// to be given explicitly in the config.
inline constexpr std::string_view kTestnetHost = "fix-test.deribit.com";
inline constexpr std::uint16_t kFixPort = 9881;

/// The one feed the Deribit backend subscribes to (full book, bid and offer).
inline constexpr std::string_view kBookFeed = "book";

}  // namespace feed_handler::deribit
