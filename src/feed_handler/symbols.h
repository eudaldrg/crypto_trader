// Small helpers over a connection's symbol list.
#pragma once

#include <span>
#include <string>

namespace feed_handler {

/// "BTC/USD,ETH/USD": the list as journaled in an connect marker.
inline std::string JoinSymbols(std::span<const std::string> symbols) {
    std::string joined;
    for (const std::string& symbol : symbols) {
        joined += joined.empty() ? "" : ",";
        joined += symbol;
    }
    return joined;
}

}  // namespace feed_handler
