#pragma once

namespace order_book {

// The one MatchingPolicy implementation for now (docs/modules/order-book.md,
// "Matching"): the feed's diffs are trusted as post-trade truth -- there is
// nothing to match, Apply() applies exactly what it is given.
class NoMatchingPolicy {
  public:
    [[nodiscard]] bool MatchingEnabled() const {
        return false;
    }
};

}  // namespace order_book
