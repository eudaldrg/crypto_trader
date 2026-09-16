// Kraken REST calls the feed handler needs before it can open a WebSocket:
// the private GetWebSocketsToken call (signed) and the public AssetPairs
// instrument reference data (cached in memory). See exchanges/kraken.md.
//
// Deliberately synchronous and startup-path only -- decisions/0004 keeps
// instrument reference data out of the streaming journal entirely.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "feed_handler/kraken/kraken_signing.h"

namespace ix {
class HttpClient;
}  // namespace ix

namespace feed_handler::kraken {

/// Live API credentials. Never logged, printed, journaled or included in any
/// error message by anything in this file.
struct credentials {
    std::string api_key;
    std::string api_secret_b64;
};

/// A WebSocket auth token. `token` is a live credential with the same handling
/// rules as the API key: it goes straight into the WS subscribe payload and
/// nowhere else (exchanges/kraken.md, and decisions/0004's journal rules).
struct websockets_token {
    std::string token;
    std::int64_t expires_seconds = 0;
};

/// Instrument reference data from GET /0/public/AssetPairs. Kraken does not
/// carry tick size or decimals inline in book messages, so this is fetched
/// once at startup and cached.
struct asset_pair {
    /// Kraken's own REST key, e.g. "XXBTZUSD".
    std::string rest_name;
    /// Short name, e.g. "XBTUSD".
    std::string altname;
    /// Kraken's WS name, e.g. "XBT/USD". NOTE: WS v2 addresses the same
    /// instrument as "BTC/USD" -- see find_asset_pair.
    std::string ws_name;
    /// Price decimals (`pair_decimals`) and quantity decimals
    /// (`lot_decimals`).
    int price_decimals = 0;
    int qty_decimals = 0;
    /// Minimum price increment, kept as the exact decimal text Kraken sent
    /// ("0.1") alongside a parsed double, so no precision is lost before the
    /// order book decides how it wants to represent prices.
    std::string tick_size;
    double tick_size_value = 0.0;
};

class rest_client {
  public:
    static constexpr std::string_view kDefaultBaseUrl = "https://api.kraken.com";
    static constexpr std::string_view kWebSocketsTokenPath = "/0/private/GetWebSocketsToken";
    static constexpr std::string_view kAssetPairsPath = "/0/public/AssetPairs";

    /// `nonce_state_file` is where the nonce high-water mark is kept across
    /// restarts; empty (the default) keeps the pre-existing purely in-memory
    /// behavior. Best effort either way -- an unusable file never stops the
    /// client from working (kraken_signing.h).
    explicit rest_client(std::string base_url = std::string(kDefaultBaseUrl),
                         std::filesystem::path nonce_state_file = {});

    rest_client(const rest_client&) = delete;
    rest_client& operator=(const rest_client&) = delete;
    rest_client(rest_client&&) = delete;
    rest_client& operator=(rest_client&&) = delete;
    ~rest_client();

    /// Signed POST to GetWebSocketsToken. A fresh token is fetched per
    /// (re)connect rather than cached: Kraken's token is short-lived if unused
    /// (exchanges/kraken.md).
    std::expected<websockets_token, std::string> fetch_websockets_token(const credentials& creds);

    /// Unauthenticated GET of the full AssetPairs table, cached in memory.
    /// `pair` optionally narrows the request to one Kraken pair name.
    std::expected<std::size_t, std::string> load_asset_pairs(std::string_view pair = {});

    /// Looks a cached pair up by REST name ("XXBTZUSD"), altname ("XBTUSD") or
    /// WS name. Both spellings of the WS name resolve: Kraken's REST `wsname`
    /// says "XBT/USD" while WS v2 subscribes to the same instrument as
    /// "BTC/USD", so BTC/XBT are treated as the same asset here.
    /// Returns nullptr if unknown. The pointer stays valid until the next
    /// load_asset_pairs() call.
    const asset_pair* find_asset_pair(std::string_view name) const;

    std::size_t cached_pair_count() const {
        return pairs_.size();
    }

    /// The nonce source backing every signed call. Exposed read-only so a test
    /// (or a startup log line) can see whether persistence is on and what mark
    /// it restored.
    const persistent_nonce_source& nonce_source() const {
        return nonce_;
    }

    /// Parses an AssetPairs response body. Exposed so the parsing can be
    /// tested against a captured response without a live network call.
    std::expected<std::size_t, std::string> parse_asset_pairs(std::string_view body);

  private:
    void index_pair(const std::string& rest_name, std::string alias);

    std::string base_url_;
    std::unique_ptr<ix::HttpClient> http_;
    persistent_nonce_source nonce_;
    std::unordered_map<std::string, asset_pair> pairs_;
    std::unordered_map<std::string, std::string> aliases_;
};

/// Uppercases `name` and rewrites the legacy "XBT" spelling to "BTC" so REST
/// and WS v2 symbol names compare equal.
std::string normalize_symbol(std::string_view name);

}  // namespace feed_handler::kraken
