#pragma once

#include <optional>

#include "order_book/book_view.h"
#include "order_book/types.h"

namespace order_book {

// Wire-shaped L1 update (decisions/0006, "Wire encoding vs. stored
// state"): a zero quantity means "no active level" at the wire level.
// L1Policy translates that into std::nullopt internally -- callers of
// Best() never see a sentinel.
struct L1Update {
    Side side;
    Price price;
    Quantity quantity;
};

// An L1 snapshot is simply the current best entry per side: L1 has no
// deeper state to reconcile incrementally, so applying one fully replaces
// the policy's state rather than being expressed as a sequence of
// L1Updates.
struct L1Snapshot {
    std::optional<BookEntry> bid;
    std::optional<BookEntry> ask;
};

// L1's ChangeSet. The engine (decisions/0006, "Engine/policy contract")
// detects this bid/ask-flag shape via if-constexpr(requires{...}) rather
// than L1Policy calling the listener directly -- the same shape is reused
// by L2Policy (T5), since a single update or batch can touch either or
// both sides.
struct L1ChangeSet {
    bool bid_top_of_book_changed = false;
    bool ask_top_of_book_changed = false;
};

// The golden L1 book (decisions/0006): no map, no order ids -- an L1 feed
// only ever carries the current best price/quantity per side, so that is
// exactly what this policy's state is, nothing more.
class L1Policy {
  public:
    [[nodiscard]] std::optional<BookEntry> Best(Side side) const {
        return side == Side::kBid ? bid_ : ask_;
    }

    // A snapshot always reflects every side's current truth, so both flags
    // are set unconditionally rather than diffed against prior state (see
    // engine.h's ApplySnapshot()).
    L1ChangeSet ApplySnapshot(const L1Snapshot& snapshot) {
        bid_ = snapshot.bid;
        ask_ = snapshot.ask;
        return L1ChangeSet{.bid_top_of_book_changed = true, .ask_top_of_book_changed = true};
    }

    L1ChangeSet Apply(const L1Update& update) {
        std::optional<BookEntry>& slot = update.side == Side::kBid ? bid_ : ask_;
        const std::optional<BookEntry> new_value =
            update.quantity.IsZero()
                ? std::nullopt
                : std::optional<BookEntry>(BookEntry{update.price, update.quantity});

        L1ChangeSet change_set;
        if (slot != new_value) {
            slot = new_value;
            (update.side == Side::kBid ? change_set.bid_top_of_book_changed
                                       : change_set.ask_top_of_book_changed) = true;
        }
        return change_set;
    }

  private:
    std::optional<BookEntry> bid_;
    std::optional<BookEntry> ask_;
};

}  // namespace order_book
