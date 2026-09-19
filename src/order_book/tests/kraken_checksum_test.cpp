#include "order_book/kraken_checksum.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "order_book/l3_policy.h"
#include "order_book/tests/kraken_documented_example.h"
#include "order_book/types.h"

namespace order_book {
namespace {

TEST(KrakenChecksum, MatchesKrakensDocumentedWorkedExample) {
    L3Policy policy;
    policy.ApplySnapshot(L3Snapshot{KrakenDocumentedExampleOrders()});

    EXPECT_EQ(KrakenL3Checksum(policy.GetBookView()), kKrakenDocumentedChecksum);
}

TEST(KrakenChecksum, EmptyBookIsWellDefined) {
    const BookView view;
    EXPECT_NO_THROW((void)KrakenL3Checksum(view));
}

// The checksum window is a property of Kraken's algorithm (top 10 levels per
// side), independent of how deep the subscription is, so a book holding more
// than that must not let the extra levels change the result.
TEST(KrakenChecksum, IgnoresLevelsBeyondItsWindow) {
    std::vector<BookEntry> window;
    for (std::int64_t level = 0; level < kKrakenChecksumLevels; ++level) {
        window.push_back(BookEntry{Price(1000 - level), Quantity(level + 1)});
    }
    std::vector<BookEntry> deeper = window;
    deeper.push_back(BookEntry{Price(1000 - kKrakenChecksumLevels), Quantity(7)});

    EXPECT_EQ(KrakenL3Checksum(BookView(window, {})), KrakenL3Checksum(BookView(deeper, {})));
}

}  // namespace
}  // namespace order_book
