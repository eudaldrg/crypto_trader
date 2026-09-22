// Turning the feed handler's config and its Kraken reference data into live
// books: what the capture binary does when `order_books = true`.
//
// This is glue, and it lives here rather than in feed_handler on purpose. The
// capture library must not depend on the book library (book_adapter depends on
// feed_handler, and decisions/0006 keeps the book code out of the capture), so
// the capture library offers two generic seams instead, and this code plugs
// books into them where both libraries are visible:
//
//   * CaptureConnection::AddSink: an extra sink on a connection's session;
//   * CaptureSet::StartAll's BeforeStart callback: called just before each
//     connection starts, with the entry and, for Kraken, the AssetPairs cache the
//     lookup has just filled.
//
// The callback is the only place both facts are available: AddSink is legal only
// before Start(), and a Kraken scale exists only after the lookup, which itself
// runs after the Deribit connections have started.
#pragma once

#include <expected>
#include <span>
#include <string>

#include "book_adapter/book_service.h"
#include "book_adapter/book_settings.h"
#include "feed_handler/capture_set.h"
#include "feed_handler/config/feed_handler_config.h"
#include "feed_handler/kraken/kraken_rest_client.h"

namespace book_adapter {

/// The check that must pass before anything starts when `config.order_books` is
/// on: every selected Deribit connection has both `price_decimals` and
/// `quantity_decimals`, since Deribit has no reference lookup to take the scale
/// from. The error names the connection and the missing keys, so a capture is
/// refused at startup instead of running without the books it was asked for.
/// Always ok when the books are off, and Kraken entries need nothing here.
std::expected<void, std::string> CheckLiveBookConfig(
    const feed_handler::config::FeedHandlerConfig& config,
    std::span<const feed_handler::config::Connection> selected);

/// What the books of one connection are built with.
///
/// Deribit: the entry's decimals (CheckLiveBookConfig makes them mandatory).
/// Kraken: the subscribed depth is the entry's `depth`, and the scale is the
/// finest decimals among the entry's symbols in `rest`'s AssetPairs cache. A
/// symbol the cache does not know is an error, never a guess from the others: a
/// wrong scale would not fail loudly, it would desync the checksum on every
/// update. `rest` may be null only for Deribit.
std::expected<BookSettings, std::string> ResolveBookSettings(
    const feed_handler::config::Connection& entry, const feed_handler::kraken::RestClient* rest);

/// The CaptureSet::StartAll callback that gives every connection a ring into
/// `service`, or an empty function when `config.order_books` is off, so a
/// capture without books touches no book code at all.
///
/// A connection whose settings cannot be resolved (a Kraken symbol missing from
/// the AssetPairs lookup, or a failed lookup) is logged and captured WITHOUT
/// books: books are an add-on and never a reason to stop or fail a capture.
///
/// `service` must outlive the CaptureSet: the sessions keep pointers to its sinks.
feed_handler::CaptureSet::BeforeStart LiveBooksHook(
    const feed_handler::config::FeedHandlerConfig& config, BookService& service);

}  // namespace book_adapter
