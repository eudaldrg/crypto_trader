#include "feed_handler/kraken/kraken_rest_client.h"

#include <ixwebsocket/IXHttpClient.h>
#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace feed_handler::kraken {
namespace {

constexpr int kConnectTimeoutSeconds = 10;
constexpr int kTransferTimeoutSeconds = 20;

/// Turns Kraken's `"error": [...]` array into a single message. Only the error
/// strings themselves are propagated -- never the rest of the body, which on a
/// successful token call contains a live credential.
std::string join_errors(simdjson::ondemand::document& doc) {
    std::string joined;
    simdjson::ondemand::array errors;
    if (doc["error"].get_array().get(errors) != simdjson::SUCCESS) {
        return joined;
    }
    for (auto entry : errors) {
        std::string_view text;
        if (entry.get_string().get(text) != simdjson::SUCCESS) {
            continue;
        }
        if (!joined.empty()) {
            joined += "; ";
        }
        joined += text;
    }
    return joined;
}

std::string describe_http_failure(std::string_view what, const ix::HttpResponsePtr& response) {
    // Deliberately no body: a GetWebSocketsToken response body carries the
    // token itself.
    return std::string(what) + ": HTTP " + std::to_string(response->statusCode) + " (" +
           response->errorMsg + ")";
}

double parse_double(std::string_view text) {
    try {
        return std::stod(std::string(text));
    } catch (const std::exception&) {
        return 0.0;
    }
}

}  // namespace

std::string normalize_symbol(std::string_view name) {
    std::string upper;
    upper.reserve(name.size());
    for (const char character : name) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    }

    // Kraken's REST reference data still spells bitcoin "XBT" (ISO 4217 style)
    // while WS v2 uses "BTC"; normalize so a WS symbol finds its REST entry.
    // Confirmed live on 2026-09-16: AssetPairs/XXBTZUSD reports
    // wsname "XBT/USD" while the level3 channel subscribes to "BTC/USD".
    std::string normalized;
    normalized.reserve(upper.size());
    std::size_t index = 0;
    while (index < upper.size()) {
        if (upper.compare(index, 3, "XBT") == 0) {
            normalized += "BTC";
            index += 3;
        } else {
            normalized.push_back(upper[index]);
            ++index;
        }
    }
    return normalized;
}

rest_client::rest_client(std::string base_url, std::filesystem::path nonce_state_file)
    : base_url_(std::move(base_url)),
      http_(std::make_unique<ix::HttpClient>(false)),
      nonce_(std::move(nonce_state_file)) {
    while (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
}

rest_client::~rest_client() = default;

std::expected<websockets_token, std::string> rest_client::fetch_websockets_token(
    const credentials& creds) {
    if (creds.api_key.empty() || creds.api_secret_b64.empty()) {
        return std::unexpected("kraken: API key/secret not set");
    }

    const std::string nonce = std::to_string(nonce_.next());
    const std::array<std::pair<std::string, std::string>, 1> params = {
        std::pair<std::string, std::string>{"nonce", nonce},
    };
    // Sign the exact body that gets sent; re-encoding it separately would risk
    // signing a different byte string than the one on the wire.
    const std::string post_data = encode_post_data(params);
    const auto signature =
        sign_private_request(kWebSocketsTokenPath, nonce, post_data, creds.api_secret_b64);
    if (!signature) {
        return std::unexpected(signature.error());
    }

    const std::string url = base_url_ + std::string(kWebSocketsTokenPath);
    auto args = http_->createRequest(url, ix::HttpClient::kPost);
    args->extraHeaders["API-Key"] = creds.api_key;
    args->extraHeaders["API-Sign"] = *signature;
    args->extraHeaders["Content-Type"] = "application/x-www-form-urlencoded";
    args->connectTimeout = kConnectTimeoutSeconds;
    args->transferTimeout = kTransferTimeoutSeconds;
    args->compress = false;

    const ix::HttpResponsePtr response = http_->post(url, post_data, args);
    if (!response || response->statusCode != 200) {
        return std::unexpected(response ? describe_http_failure("GetWebSocketsToken", response)
                                        : std::string("GetWebSocketsToken: no response"));
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string json(response->body);
    simdjson::ondemand::document doc;
    if (parser.iterate(json).get(doc) != simdjson::SUCCESS) {
        return std::unexpected("GetWebSocketsToken: unparseable response body (" +
                               std::to_string(response->body.size()) + " bytes)");
    }

    const std::string errors = join_errors(doc);
    if (!errors.empty()) {
        return std::unexpected("GetWebSocketsToken: " + errors);
    }

    simdjson::ondemand::object result;
    if (doc["result"].get_object().get(result) != simdjson::SUCCESS) {
        return std::unexpected("GetWebSocketsToken: response has no result object");
    }

    websockets_token token;
    std::string_view raw_token;
    if (result["token"].get_string().get(raw_token) != simdjson::SUCCESS) {
        return std::unexpected("GetWebSocketsToken: response has no token field");
    }
    token.token.assign(raw_token);
    std::int64_t expires = 0;
    if (result["expires"].get_int64().get(expires) == simdjson::SUCCESS) {
        token.expires_seconds = expires;
    }
    return token;
}

std::expected<std::size_t, std::string> rest_client::load_asset_pairs(std::string_view pair) {
    std::string url = base_url_ + std::string(kAssetPairsPath);
    if (!pair.empty()) {
        url += "?pair=" + url_encode(pair);
    }

    auto args = http_->createRequest(url, ix::HttpClient::kGet);
    args->connectTimeout = kConnectTimeoutSeconds;
    args->transferTimeout = kTransferTimeoutSeconds;
    args->compress = false;

    const ix::HttpResponsePtr response = http_->get(url, args);
    if (!response || response->statusCode != 200) {
        return std::unexpected(response ? describe_http_failure("AssetPairs", response)
                                        : std::string("AssetPairs: no response"));
    }
    return parse_asset_pairs(response->body);
}

std::expected<std::size_t, std::string> rest_client::parse_asset_pairs(std::string_view body) {
    simdjson::ondemand::parser parser;
    simdjson::padded_string json(body);
    simdjson::ondemand::document doc;
    if (parser.iterate(json).get(doc) != simdjson::SUCCESS) {
        return std::unexpected("AssetPairs: unparseable response body");
    }

    const std::string errors = join_errors(doc);
    if (!errors.empty()) {
        return std::unexpected("AssetPairs: " + errors);
    }

    simdjson::ondemand::object result;
    if (doc["result"].get_object().get(result) != simdjson::SUCCESS) {
        return std::unexpected("AssetPairs: response has no result object");
    }

    pairs_.clear();
    aliases_.clear();

    for (auto field : result) {
        std::string_view key;
        if (field.unescaped_key().get(key) != simdjson::SUCCESS) {
            continue;
        }
        simdjson::ondemand::object entry;
        if (field.value().get_object().get(entry) != simdjson::SUCCESS) {
            continue;
        }

        asset_pair parsed;
        parsed.rest_name.assign(key);

        // Read in the order Kraken emits the fields: simdjson's on-demand
        // cursor is cheapest when it never has to rewind.
        std::string_view text;
        if (entry["altname"].get_string().get(text) == simdjson::SUCCESS) {
            parsed.altname.assign(text);
        }
        if (entry["wsname"].get_string().get(text) == simdjson::SUCCESS) {
            parsed.ws_name.assign(text);
        }
        std::int64_t number = 0;
        if (entry["pair_decimals"].get_int64().get(number) == simdjson::SUCCESS) {
            parsed.price_decimals = static_cast<int>(number);
        }
        if (entry["lot_decimals"].get_int64().get(number) == simdjson::SUCCESS) {
            parsed.qty_decimals = static_cast<int>(number);
        }
        if (entry["tick_size"].get_string().get(text) == simdjson::SUCCESS) {
            parsed.tick_size.assign(text);
            parsed.tick_size_value = parse_double(parsed.tick_size);
        }

        const std::string rest_name = parsed.rest_name;
        pairs_.emplace(rest_name, std::move(parsed));

        const asset_pair& stored = pairs_.at(rest_name);
        index_pair(rest_name, rest_name);
        index_pair(rest_name, stored.altname);
        index_pair(rest_name, stored.ws_name);
    }

    return pairs_.size();
}

void rest_client::index_pair(const std::string& rest_name, std::string alias) {
    if (alias.empty()) {
        return;
    }
    aliases_.insert_or_assign(alias, rest_name);
    aliases_.insert_or_assign(normalize_symbol(alias), rest_name);
}

const asset_pair* rest_client::find_asset_pair(std::string_view name) const {
    const auto direct = aliases_.find(std::string(name));
    if (direct != aliases_.end()) {
        const auto pair = pairs_.find(direct->second);
        return pair == pairs_.end() ? nullptr : &pair->second;
    }
    const auto normalized = aliases_.find(normalize_symbol(name));
    if (normalized == aliases_.end()) {
        return nullptr;
    }
    const auto pair = pairs_.find(normalized->second);
    return pair == pairs_.end() ? nullptr : &pair->second;
}

}  // namespace feed_handler::kraken
