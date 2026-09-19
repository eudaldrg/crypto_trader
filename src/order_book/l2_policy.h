#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "order_book/book_view.h"
#include "order_book/types.h"

namespace order_book {

using ChangeId = Ordered<struct ChangeIdTag, std::int64_t>;

// Deribit's WS book wire shape specifically (decisions/0006, decisions/
// 0001): each entry is an explicit [operation, price, quantity] tuple --
// confirmed against a real captured session (2026-09-17): New/Change both
// carry the level's new total quantity, Delete always carries quantity
// 0 (an artifact of nothing being left, not the delete signal itself).
// Relying on quantity==0 to mean "delete" was tried in an earlier draft
// of this policy and technically worked against real data (delete always
// happens to carry 0), but that's an unstated invariant, not what the
// protocol actually signals -- the explicit operation is what's used
// here. FIX's MDUpdateAction-based L2 shape is a distinct wire shape the
// feed handler normalizes into this one -- out of scope here.
enum class L2Operation : std::uint8_t { kNew, kChange, kDelete };

struct L2Update {
    Side side;
    Price price;
    Quantity quantity;
    L2Operation operation;
};

// Validated once per batch, not per update (decisions/0006, "Message/
// batch boundary"): a real book message carries several updates under
// one change_id.
struct ChangeIdMeta {
    ChangeId change_id;
    ChangeId prev_change_id;
};

// A snapshot's levels are plain L2Update-shaped entries (side, price,
// quantity) covering both sides; the snapshot itself establishes the
// change_id baseline subsequent apply_batch calls are validated against.
struct L2Snapshot {
    std::vector<L2Update> levels;
    ChangeId change_id;
};

struct L2ChangeSet {
    bool bid_top_of_book_changed = false;
    bool ask_top_of_book_changed = false;
    std::vector<LevelChange> level_changes;
    std::optional<IntegrityIssue> integrity_issue;
};

// The golden L2 book (decisions/0006): aggregated price levels per side,
// with batch-level change_id sequencing and crossed-book detection. A
// genuine gap or a crossed book after applying a batch is reported via
// L2ChangeSet::integrity_issue -- the engine turns that into a Desynced
// transition, so this policy never has to "keep going" after either.
class L2Policy {
  public:
    [[nodiscard]] std::optional<BookEntry> Best(Side side) const {
        return side == Side::kBid ? BestOf(bids_) : BestOf(asks_);
    }

    // Returns a real ChangeSet (rather than void) so a snapshot goes through
    // the same engine-side notify/readiness machinery as an ordinary batch:
    // a LevelListener sees the snapshot's initial levels via OnLevelChanged,
    // and a crossed-book snapshot is reported and desyncs the book instead
    // of being applied silently.
    L2ChangeSet ApplySnapshot(const L2Snapshot& snapshot) {
        bids_.clear();
        asks_.clear();
        L2ChangeSet change_set;
        for (const L2Update& level : snapshot.levels) {
            if (level.operation == L2Operation::kDelete) {
                continue;
            }
            if (level.side == Side::kBid) {
                bids_.insert_or_assign(level.price, level.quantity);
            } else {
                asks_.insert_or_assign(level.price, level.quantity);
            }
            change_set.level_changes.push_back({level.side, level.price, level.quantity});
        }
        last_change_id_ = snapshot.change_id;

        change_set.bid_top_of_book_changed = true;
        change_set.ask_top_of_book_changed = true;
        if (IsCrossed(Best(Side::kBid), Best(Side::kAsk))) {
            ReportIssue(change_set.integrity_issue, IntegrityIssue::kCrossedBook);
        }
        return change_set;
    }

    L2ChangeSet ApplyBatch(std::span<const L2Update> updates, const ChangeIdMeta& meta) {
        L2ChangeSet change_set;

        // A genuine gap means the state after it is unknown -- don't guess at
        // applying updates that may not be the ones the exchange actually
        // meant to follow the last message we saw. The engine transitions to
        // Desynced from integrity_issue alone, so state here stays untouched.
        if (meta.prev_change_id != last_change_id_) {
            change_set.integrity_issue = IntegrityIssue::kGap;
            return change_set;
        }

        const std::optional<BookEntry> old_best_bid = Best(Side::kBid);
        const std::optional<BookEntry> old_best_ask = Best(Side::kAsk);

        for (const L2Update& update : updates) {
            if (update.side == Side::kBid) {
                ApplyOne(bids_, Side::kBid, update, change_set);
            } else {
                ApplyOne(asks_, Side::kAsk, update, change_set);
            }
        }

        const std::optional<BookEntry> new_best_bid = Best(Side::kBid);
        const std::optional<BookEntry> new_best_ask = Best(Side::kAsk);
        change_set.bid_top_of_book_changed = old_best_bid != new_best_bid;
        change_set.ask_top_of_book_changed = old_best_ask != new_best_ask;

        if (IsCrossed(new_best_bid, new_best_ask)) {
            ReportIssue(change_set.integrity_issue, IntegrityIssue::kCrossedBook);
        }

        last_change_id_ = meta.change_id;
        return change_set;
    }

  private:
    template <typename Map>
    static std::optional<BookEntry> BestOf(const Map& map) {
        if (map.empty()) {
            return std::nullopt;
        }
        return BookEntry{map.begin()->first, map.begin()->second};
    }

    template <typename Map>
    static void ApplyOne(Map& map, Side side, const L2Update& update, L2ChangeSet& change_set) {
        if (update.operation == L2Operation::kDelete) {
            if (map.erase(update.price) == 0) {
                ReportIssue(change_set.integrity_issue, IntegrityIssue::kUnknownLevel);
                return;
            }
            change_set.level_changes.push_back({side, update.price, std::nullopt});
            return;
        }
        const bool already_tracked = map.find(update.price) != map.end();
        if (update.operation == L2Operation::kChange && !already_tracked) {
            ReportIssue(change_set.integrity_issue, IntegrityIssue::kUnknownLevel);
            return;
        }
        assert(!(update.operation == L2Operation::kNew && already_tracked) &&
               "duplicate New for a price level already tracked (decisions/0006, tier-2)");
        map.insert_or_assign(update.price, update.quantity);
        change_set.level_changes.push_back({side, update.price, update.quantity});
    }

    LevelMap<Price, Quantity, std::greater<>> bids_;
    LevelMap<Price, Quantity, std::less<>> asks_;
    ChangeId last_change_id_{};
};

}  // namespace order_book
