#include "book_adapter/book_wiring.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "feed_handler/logging.h"
#include "order_book/instrument_scale.h"

namespace book_adapter {

namespace {

using feed_handler::config::Connection;
using feed_handler::config::Exchange;

std::expected<BookSettings, std::string> ResolveDeribit(const Connection& entry) {
    if (!entry.price_decimals.has_value() || !entry.quantity_decimals.has_value()) {
        return std::unexpected(
            "deribit needs both price_decimals and quantity_decimals to build "
            "books: it has no instrument lookup to take the scale from");
    }
    return BookSettings{
        .source = feed_handler::FrameSource::kDeribitFix,
        .scale = order_book::InstrumentScale(*entry.price_decimals, *entry.quantity_decimals),
    };
}

std::expected<BookSettings, std::string> ResolveKraken(
    const Connection& entry, const feed_handler::kraken::RestClient* rest) {
    if (rest == nullptr) {
        return std::unexpected("no Kraken instrument lookup is available");
    }
    int price_decimals = 0;
    int quantity_decimals = 0;
    for (const std::string& symbol : entry.symbols) {
        const feed_handler::kraken::AssetPair* pair = rest->FindAssetPair(symbol);
        if (pair == nullptr) {
            return std::unexpected("no AssetPairs entry for " + symbol +
                                   " (the lookup failed, or the symbol is misspelled)");
        }
        if (pair->price_decimals < 0 || pair->qty_decimals < 0 ||
            pair->price_decimals > feed_handler::config::kMaxDecimals ||
            pair->qty_decimals > feed_handler::config::kMaxDecimals) {
            return std::unexpected("AssetPairs decimals for " + symbol + " are out of range");
        }
        // One scale serves every symbol on the connection, so it has to be fine
        // enough for the finest of them.
        price_decimals = std::max(price_decimals, pair->price_decimals);
        quantity_decimals = std::max(quantity_decimals, pair->qty_decimals);
    }
    return BookSettings{
        .source = feed_handler::FrameSource::kKrakenJson,
        .scale = order_book::InstrumentScale(price_decimals, quantity_decimals),
        .kraken_depth = static_cast<std::size_t>(entry.depth),
    };
}

}  // namespace

std::expected<void, std::string> CheckLiveBookConfig(
    const feed_handler::config::FeedHandlerConfig& config,
    std::span<const feed_handler::config::Connection> selected) {
    if (!config.order_books) {
        return {};
    }
    for (const Connection& entry : selected) {
        if (entry.exchange != Exchange::kDeribit) {
            continue;
        }
        std::string missing;
        if (!entry.price_decimals.has_value()) {
            missing += "price_decimals";
        }
        if (!entry.quantity_decimals.has_value()) {
            missing += std::string(missing.empty() ? "" : " and ") + "quantity_decimals";
        }
        if (!missing.empty()) {
            return std::unexpected("[" + entry.id + "] order_books is on but this deribit " +
                                   "connection has no " + missing +
                                   "; set them to the decimals of its instruments (a book "
                                   "cannot be built without a scale)");
        }
    }
    return {};
}

std::expected<BookSettings, std::string> ResolveBookSettings(
    const Connection& entry, const feed_handler::kraken::RestClient* rest) {
    switch (entry.exchange) {
        case Exchange::kKraken:
            return ResolveKraken(entry, rest);
        case Exchange::kDeribit:
            return ResolveDeribit(entry);
    }
    return std::unexpected("no book support for this exchange");
}

feed_handler::CaptureSet::BeforeStart LiveBooksHook(
    const feed_handler::config::FeedHandlerConfig& config, BookService& service) {
    if (!config.order_books) {
        return {};
    }
    return [&service](feed_handler::CaptureConnection& connection, const Connection& entry,
                      const feed_handler::kraken::RestClient* rest) {
        const feed_handler::TaggedLog log(entry.id);
        const auto settings = ResolveBookSettings(entry, rest);
        if (!settings) {
            log.Error("order books off for this connection, capture continues without them: " +
                      settings.error());
            return;
        }
        connection.AddSink(service.AddConnection(entry.id, *settings));
        log.Info("order books on");
    };
}

}  // namespace book_adapter
