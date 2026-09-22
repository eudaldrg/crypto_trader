// Turns one journaled Deribit FIX message into what an L2 book applies
// (exchanges/deribit.md, "Responses"). The FIX counterpart of
// order_book/kraken_l3_wire.h, kept in the adapter library rather than the
// order-book one because it reads feed_handler's FIX parser, and decisions/0006
// keeps order_book free of feed_handler.
//
//   35=W  MarketDataSnapshotFullRefresh: symbol from 55, one entry per level
//         keyed by 269 (0 bid, 1 offer) with 270 price and 271 size, in an
//         UnsequencedL2Snapshot.
//   35=X  MarketDataIncrementalRefresh: entries keyed by 279 (0 New, 1 Change,
//         2 Delete) with 269/270/271, as one batch of L2Update for
//         UnsequencedL2Policy. FIX carries no change_id (decisions/0006, "L2
//         without a change_id"), so there is no meta.
//   anything else (35=A, 0, 1, 5, 3, j, Y, ...): session traffic, nothing for a
//         book, reported as kIgnored.
//
// The two entry shapes differ (279 leads a 35=X entry and is absent from a
// 35=W one), so each is read with its own member tags.
//
// One journal record is one complete FIX message: the client frames the stream
// before journaling (deribit_capture.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "order_book/instrument_scale.h"
#include "order_book/l2_policy.h"

namespace book_adapter {

enum class FixBookMessageKind : std::uint8_t {
    /// Not market data for a book: a session message, or any type the books do
    /// not use. Nothing else in the result is meaningful.
    kIgnored,
    kSnapshot,
    kUpdate,
};

struct DeribitFixBookMessage {
    FixBookMessageKind kind = FixBookMessageKind::kIgnored;
    /// The instrument, tag 55. Set for kSnapshot and kUpdate.
    std::string symbol;
    /// kSnapshot only: every entry, as New.
    order_book::UnsequencedL2Snapshot snapshot;
    /// kUpdate only: the message's entries in wire order, applied as one batch.
    std::vector<order_book::L2Update> updates;
};

/// Why a message could not be used. Never contains field values (a malformed
/// Logon carries credentials), only tag numbers and entry positions.
struct DeribitFixParseError {
    std::string what;
    /// The instrument, when the message got far enough to name it. Empty when
    /// it did not (a corrupt envelope, no tag 55): the caller then cannot say
    /// which book the message was for.
    std::string symbol;
};

/// Parses `payload`, one complete FIX message, using `scale` to turn 270 and 271
/// into ticks and lots.
///
/// Malformed means a failed envelope check (BodyLength, CheckSum), a 35=W or
/// 35=X without a symbol or with no NoMDEntries group, a group that ends before
/// the count it declared, or an entry with a missing or unusable member: an
/// entry type other than 0/1, an update action other than 0/1/2, a price or size
/// that is not a finite decimal, a negative size. The size of a Delete is
/// optional (it means nothing there). A malformed message is an error as a
/// whole, never a partial batch: applying part of it would leave a book that
/// matches neither the state before nor after.
[[nodiscard]] std::expected<DeribitFixBookMessage, DeribitFixParseError> ParseDeribitFixMessage(
    std::span<const std::byte> payload, const order_book::InstrumentScale& scale);

}  // namespace book_adapter
