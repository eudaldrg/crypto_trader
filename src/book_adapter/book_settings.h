// Per-connection settings a BookAdapter needs to turn a connection's frames
// into books. They are properties of the connection's subscription and of the
// instruments on it, not of the book library, so the caller (the live wiring
// from the config, or the replay tool from its command line) supplies them.
#pragma once

#include <cstddef>

#include "feed_handler/kraken/kraken_endpoints.h"
#include "feed_handler/message_sink.h"
#include "order_book/instrument_scale.h"

namespace book_adapter {

/// Kraken's level3 default depth, what a subscription that names none gets.
/// Same fact as feed_handler::kraken::kDefaultDepth, kept as one constant so
/// the book's default and the subscription's default cannot drift apart.
inline constexpr std::size_t kDefaultKrakenDepth =
    static_cast<std::size_t>(feed_handler::kraken::kDefaultDepth);

struct BookSettings {
    /// What shape the connection's frame payloads are in, so which parser and
    /// which book they go to. The adapter trusts this rather than
    /// CaptureFrame::source: a journal does not record the source per record
    /// (message_sink.h), so a replayed frame arrives as kUnknown.
    feed_handler::FrameSource source;

    /// Price and quantity decimals, applied to every symbol on the connection.
    /// Kraken: from the AssetPairs lookup. Deribit: from the config.
    order_book::InstrumentScale scale;

    /// Kraken only: the depth the subscription asked for. Must equal it, or the
    /// book desyncs its checksum silently (kraken_l3_policy.h).
    std::size_t kraken_depth = kDefaultKrakenDepth;
};

}  // namespace book_adapter
