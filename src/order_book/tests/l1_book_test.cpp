#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "order_book/engine.h"
#include "order_book/l1_policy.h"
#include "order_book/no_matching_policy.h"
#include "order_book/types.h"

namespace order_book {
namespace {

struct RecordingListener {
    struct Notification {
        Side side;
        std::optional<BookEntry> best;
    };
    std::vector<Notification> notifications;

    void OnTopOfBookChanged(Side side, std::optional<BookEntry> best) {
        notifications.push_back({side, best});
    }
};

using L1Book = OrderBook<L1Policy, RecordingListener>;

TEST(L1Book, NotReadyBeforeSnapshot) {
    RecordingListener listener;
    L1Book book(listener);

    EXPECT_FALSE(book.IsReady());
    EXPECT_EQ(book.GetReadiness(), Readiness::kUninitialized);
}

TEST(L1Book, ReadyAfterSnapshot) {
    RecordingListener listener;
    L1Book book(listener);

    book.ApplySnapshot(L1Snapshot{});

    EXPECT_TRUE(book.IsReady());
    EXPECT_EQ(book.GetReadiness(), Readiness::kReady);
}

TEST(L1Book, SnapshotNotifiesBothSides) {
    RecordingListener listener;
    L1Book book(listener);

    book.ApplySnapshot(L1Snapshot{
        .bid = BookEntry{Price(100), Quantity(5)},
        .ask = BookEntry{Price(101), Quantity(3)},
    });

    ASSERT_EQ(listener.notifications.size(), 2U);
    EXPECT_EQ(listener.notifications[0].side, Side::kBid);
    EXPECT_EQ(listener.notifications[0].best, (BookEntry{Price(100), Quantity(5)}));
    EXPECT_EQ(listener.notifications[1].side, Side::kAsk);
    EXPECT_EQ(listener.notifications[1].best, (BookEntry{Price(101), Quantity(3)}));
}

TEST(L1Book, NewBestAppears) {
    RecordingListener listener;
    L1Book book(listener);
    book.ApplySnapshot(L1Snapshot{});
    listener.notifications.clear();

    book.Apply(L1Update{Side::kBid, Price(100), Quantity(5)});

    ASSERT_EQ(listener.notifications.size(), 1U);
    EXPECT_EQ(listener.notifications[0].side, Side::kBid);
    EXPECT_EQ(listener.notifications[0].best, (BookEntry{Price(100), Quantity(5)}));
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(5)}));
}

TEST(L1Book, BetterPriceReplacesPriorBest) {
    RecordingListener listener;
    L1Book book(listener);
    book.ApplySnapshot(L1Snapshot{.bid = BookEntry{Price(100), Quantity(5)}, .ask = std::nullopt});
    listener.notifications.clear();

    book.Apply(L1Update{Side::kBid, Price(101), Quantity(5)});

    ASSERT_EQ(listener.notifications.size(), 1U);
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(101), Quantity(5)}));
}

TEST(L1Book, QuantityOnlyChangeAtSamePriceStillNotifies) {
    RecordingListener listener;
    L1Book book(listener);
    book.ApplySnapshot(L1Snapshot{.bid = BookEntry{Price(100), Quantity(5)}, .ask = std::nullopt});
    listener.notifications.clear();

    book.Apply(L1Update{Side::kBid, Price(100), Quantity(9)});

    ASSERT_EQ(listener.notifications.size(), 1U);
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(9)}));
}

TEST(L1Book, DeletingBestLevelClearsAndNotifies) {
    RecordingListener listener;
    L1Book book(listener);
    book.ApplySnapshot(L1Snapshot{.bid = BookEntry{Price(100), Quantity(5)}, .ask = std::nullopt});
    listener.notifications.clear();

    book.Apply(L1Update{Side::kBid, Price(100), Quantity(0)});

    ASSERT_EQ(listener.notifications.size(), 1U);
    EXPECT_EQ(listener.notifications[0].best, std::nullopt);
    EXPECT_EQ(book.Best(Side::kBid), std::nullopt);
}

TEST(L1Book, ApplyBeforeReadyIsANoOp) {
    RecordingListener listener;
    L1Book book(listener);

    book.Apply(L1Update{Side::kBid, Price(100), Quantity(5)});

    EXPECT_TRUE(listener.notifications.empty());
    EXPECT_EQ(book.Best(Side::kBid), std::nullopt);
}

// --- MatchingPolicy concept ---

struct FakeMatchingPolicy {
    [[nodiscard]] bool MatchingEnabled() const {
        return true;
    }
};
static_assert(MatchingPolicyConcept<FakeMatchingPolicy>);
static_assert(MatchingPolicyConcept<NoMatchingPolicy>);

TEST(L1Book, AcceptsFakeMatchingPolicyWithoutInheritance) {
    RecordingListener listener;
    OrderBook<L1Policy, RecordingListener, FakeMatchingPolicy> book(listener);

    EXPECT_TRUE(book.MatchingEnabled());
}

TEST(L1Book, DefaultMatchingPolicyReportsDisabled) {
    RecordingListener listener;
    L1Book book(listener);

    EXPECT_FALSE(book.MatchingEnabled());
}

}  // namespace
}  // namespace order_book
