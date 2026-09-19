#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "order_book/book_view.h"
#include "order_book/kraken_checksum.h"
#include "order_book/l3_policy.h"
#include "order_book/types.h"

namespace order_book {

// The per-message `checksum` Kraken sends on every level3 snapshot and update.
struct ChecksumMeta {
    std::uint32_t checksum;
};

// An L3Update plus the one field only Kraken's feed needs: the order's own
// RFC3339 timestamp, exactly as sent (fixed-width, UTC, nanoseconds, so it
// sorts correctly as a plain string).
struct KrakenL3Update {
    L3Update update;
    std::string timestamp;
};

// Kraken's level3 feed on top of the generic L3Policy (decisions/0006). Two
// things here are Kraken's and not the generic book's:
//
//  * Checksum verification. Every message carries a checksum of the top-10
//    book that the client is expected to recompute; a mismatch is reported as
//    kChecksumMismatch. Kraken has no sequence numbers, so this is the only
//    desync detection the feed offers.
//  * Add ordering. The JSON array order of one message's entries does not
//    always match their true arrival order (confirmed against a real capture,
//    exchanges/kraken.md), and queue priority -- and therefore the checksum --
//    depends on true arrival order. Adds are reordered by their own timestamp.
//
// subscribed_depth must equal the depth the subscription requested (Kraken's
// default, 10, if the subscribe message carries none): Kraken sends no delete
// for a level that falls out of it, so the generic book has to drop those
// itself. It is deliberately not defaulted -- getting it wrong desyncs the
// checksum silently. The checksum's own window is always the top 10 levels
// regardless of subscribed_depth (see kraken_checksum.h).
class KrakenL3Policy {
  public:
    explicit KrakenL3Policy(std::size_t subscribed_depth) : book_(subscribed_depth) {}

    [[nodiscard]] std::optional<BookEntry> Best(Side side) const {
        return book_.Best(side);
    }

    // The underlying generic book, for queries (BookView, Levels, OrderQuantity).
    [[nodiscard]] const L3Policy& Book() const {
        return book_;
    }

    L3ChangeSet ApplySnapshot(const L3Snapshot& snapshot, const ChecksumMeta& meta) {
        L3ChangeSet change_set = book_.ApplySnapshot(snapshot);
        VerifyChecksum(change_set, meta);
        return change_set;
    }

    L3ChangeSet ApplyBatch(std::span<const KrakenL3Update> updates, const ChecksumMeta& meta) {
        L3ChangeSet change_set = book_.ApplyBatch(AddsInArrivalOrder(updates));
        VerifyChecksum(change_set, meta);
        return change_set;
    }

  private:
    // Only the Add events are reordered among themselves, by timestamp, into
    // the positions Add events originally occupied; every non-Add event keeps
    // its position relative to everything else. That matters: one message can
    // carry an Add followed by a Modify/Delete of the same order_id, and
    // hoisting all non-Adds ahead of all Adds would apply that later event
    // before the order it targets exists.
    static std::vector<L3Update> AddsInArrivalOrder(std::span<const KrakenL3Update> updates) {
        std::vector<const KrakenL3Update*> adds;
        for (const KrakenL3Update& entry : updates) {
            if (entry.update.event == OrderEventKind::kAdded) {
                adds.push_back(&entry);
            }
        }
        std::stable_sort(adds.begin(), adds.end(),
                         [](const KrakenL3Update* lhs, const KrakenL3Update* rhs) {
                             return lhs->timestamp < rhs->timestamp;
                         });

        std::vector<L3Update> ordered;
        ordered.reserve(updates.size());
        auto next_add = adds.begin();
        for (const KrakenL3Update& entry : updates) {
            ordered.push_back(entry.update.event == OrderEventKind::kAdded ? (*next_add++)->update
                                                                           : entry.update);
        }
        return ordered;
    }

    // An earlier UnknownOrder/CrossedBook is kept (ReportIssue): a checksum
    // mismatch after one of those is expected, since the book is already known
    // to be wrong. Only the checksum's own window is copied out of the book.
    void VerifyChecksum(L3ChangeSet& change_set, const ChecksumMeta& meta) const {
        if (KrakenL3Checksum(book_.GetBookView(static_cast<std::size_t>(kKrakenChecksumLevels))) !=
            meta.checksum) {
            ReportIssue(change_set.integrity_issue, IntegrityIssue::kChecksumMismatch);
        }
    }

    L3Policy book_;
};

}  // namespace order_book
