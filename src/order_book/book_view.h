#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "order_book/types.h"

namespace order_book {

// One priced entry in a BookView: for an L2 view this is one aggregated
// price level; for an L3 view this is one individual order (so the same
// price can appear more than once, once per resting order at it) -- the
// shape a per-order checksum needs is a superset of the shape a per-level
// one needs, so one entry type covers both.
struct BookEntry {
    Price price;
    Quantity quantity;

    friend bool operator==(const BookEntry&, const BookEntry&) = default;
};

// The golden book's oracle surface (decisions/0006, "Query surface and the
// BookView oracle"): a granularity-agnostic, comparable snapshot
// of one side's visible entries, best-to-worst. Populated by whichever
// GranularityPolicy is asked for it -- BookView itself has no opinion on
// how many entries there are or what they represent.
class BookView {
  public:
    BookView() = default;
    BookView(std::vector<BookEntry> bids, std::vector<BookEntry> asks)
        : bids_(std::move(bids)), asks_(std::move(asks)) {}

    [[nodiscard]] const std::vector<BookEntry>& Entries(Side side) const {
        return side == Side::kBid ? bids_ : asks_;
    }

    friend bool operator==(const BookView&, const BookView&) = default;

  private:
    std::vector<BookEntry> bids_;
    std::vector<BookEntry> asks_;
};

// True when the best bid is at or above the best ask, which no valid book can be.
inline bool IsCrossed(const std::optional<BookEntry>& best_bid,
                      const std::optional<BookEntry>& best_ask) {
    return best_bid.has_value() && best_ask.has_value() && best_bid->price >= best_ask->price;
}

}  // namespace order_book
