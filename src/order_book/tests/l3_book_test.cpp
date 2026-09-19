#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "order_book/engine.h"
#include "order_book/l3_policy.h"
#include "order_book/tests/l3_test_util.h"
#include "order_book/types.h"

namespace order_book {
namespace {

struct RecordingListener {
    struct TopOfBook {
        Side side;
        std::optional<BookEntry> best;
    };

    std::vector<TopOfBook> top_of_book;
    std::vector<LevelChange> level_changes;
    std::vector<OrderEvent> order_events;
    std::vector<IntegrityIssue> integrity_issues;

    void OnTopOfBookChanged(Side side, std::optional<BookEntry> best) {
        top_of_book.push_back({side, best});
    }
    void OnLevelChanged(Side side, Price price, Quantity quantity) {
        level_changes.push_back(LevelChange{side, price, quantity});
    }
    void OnLevelDeleted(Side side, Price price) {
        level_changes.push_back(LevelChange{side, price, std::nullopt});
    }
    void OnOrderAdded(OrderId order_id, Side side, Price price, Quantity quantity) {
        order_events.push_back(OrderEvent{OrderEventKind::kAdded, order_id, side, price, quantity});
    }
    void OnOrderModified(OrderId order_id, Quantity quantity) {
        order_events.push_back(
            OrderEvent{OrderEventKind::kModified, order_id, Side::kBid, Price{}, quantity});
    }
    void OnOrderDeleted(OrderId order_id) {
        order_events.push_back(
            OrderEvent{OrderEventKind::kDeleted, order_id, Side::kBid, Price{}, Quantity{}});
    }
    void OnIntegrityCheckFailed(IntegrityIssue issue) {
        integrity_issues.push_back(issue);
    }
};

using L3Book = OrderBook<L3Policy, RecordingListener>;

using l3_test::Add;
using l3_test::Delete;
using l3_test::Modify;

void ApplyBatch(L3Book& book, const std::vector<L3Update>& updates) {
    book.ApplyBatch(std::span<const L3Update>(updates));
}

TEST(L3Book, AddsNewOrder) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{});
    listener.top_of_book.clear();

    ApplyBatch(book, {Add(1, Side::kBid, 100, 5)});

    ASSERT_EQ(listener.order_events.size(), 1U);
    EXPECT_EQ(listener.order_events[0].kind, OrderEventKind::kAdded);
    EXPECT_EQ(listener.order_events[0].order_id, OrderId(1));
    ASSERT_EQ(listener.level_changes.size(), 1U);
    EXPECT_EQ(listener.level_changes[0].quantity, Quantity(5));
    ASSERT_EQ(listener.top_of_book.size(), 1U);
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(5)}));
}

TEST(L3Book, MultipleOrdersAtSamePriceAggregateInArrivalOrder) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{});

    ApplyBatch(book, {Add(1, Side::kBid, 100, 5), Add(2, Side::kBid, 100, 3)});

    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(8)}));
    const BookView view = book.Policy().GetBookView();
    const std::vector<BookEntry>& bid_entries = view.Entries(Side::kBid);
    ASSERT_EQ(bid_entries.size(), 2U);
    EXPECT_EQ(bid_entries[0], (BookEntry{Price(100), Quantity(5)}));
    EXPECT_EQ(bid_entries[1], (BookEntry{Price(100), Quantity(3)}));
}

TEST(L3Book, ModifiesOrderQuantity) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{});
    ApplyBatch(book, {Add(1, Side::kBid, 100, 5)});
    listener.order_events.clear();
    listener.level_changes.clear();

    ApplyBatch(book, {Modify(1, 9)});

    ASSERT_EQ(listener.order_events.size(), 1U);
    EXPECT_EQ(listener.order_events[0].kind, OrderEventKind::kModified);
    EXPECT_EQ(listener.order_events[0].quantity, Quantity(9));
    ASSERT_EQ(listener.level_changes.size(), 1U);
    EXPECT_EQ(listener.level_changes[0].quantity, Quantity(9));
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(9)}));
}

TEST(L3Book, DeletingOneOfTwoOrdersReducesLevelAggregate) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{});
    ApplyBatch(book, {Add(1, Side::kBid, 100, 5), Add(2, Side::kBid, 100, 3)});
    listener.order_events.clear();
    listener.level_changes.clear();

    ApplyBatch(book, {Delete(1)});

    ASSERT_EQ(listener.order_events.size(), 1U);
    EXPECT_EQ(listener.order_events[0].kind, OrderEventKind::kDeleted);
    ASSERT_EQ(listener.level_changes.size(), 1U);
    EXPECT_EQ(listener.level_changes[0].quantity, Quantity(3));
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(3)}));
}

TEST(L3Book, DeletingLastOrderAtPriceRemovesLevel) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{});
    ApplyBatch(book, {Add(1, Side::kBid, 100, 5)});
    listener.level_changes.clear();
    listener.top_of_book.clear();

    ApplyBatch(book, {Delete(1)});

    ASSERT_EQ(listener.level_changes.size(), 1U);
    EXPECT_EQ(listener.level_changes[0].quantity, std::nullopt);
    ASSERT_EQ(listener.top_of_book.size(), 1U);
    EXPECT_EQ(listener.top_of_book[0].best, std::nullopt);
    EXPECT_EQ(book.Best(Side::kBid), std::nullopt);
}

TEST(L3Book, DeleteOfUnknownOrderIdReportsUnknownOrder) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{});

    ApplyBatch(book, {Delete(999)});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kUnknownOrder);
    EXPECT_EQ(book.GetReadiness(), Readiness::kDesynced);
}

TEST(L3Book, ModifyOfUnknownOrderIdReportsUnknownOrder) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{});

    ApplyBatch(book, {Modify(999, 1)});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kUnknownOrder);
}

TEST(L3Book, CrossedBookAfterBatchReportsIntegrityIssue) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{{Add(1, Side::kBid, 100, 5), Add(2, Side::kAsk, 101, 5)}});
    listener.integrity_issues.clear();

    ApplyBatch(book, {Add(3, Side::kBid, 102, 5)});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kCrossedBook);
    EXPECT_EQ(book.GetReadiness(), Readiness::kDesynced);
}

// A snapshot goes through the same ChangeSet path as a batch: a listener that
// tracks orders sees the snapshot's resting orders, and a malformed snapshot is
// reported instead of being applied silently.
TEST(L3Book, SnapshotNotifiesOrderListenerOfRestingOrders) {
    RecordingListener listener;
    L3Book book(listener);

    book.ApplySnapshot(L3Snapshot{{Add(1, Side::kBid, 100, 5), Add(2, Side::kAsk, 101, 3)}});

    ASSERT_EQ(listener.order_events.size(), 2U);
    EXPECT_EQ(listener.order_events[0].order_id, OrderId(1));
    EXPECT_EQ(listener.order_events[1].order_id, OrderId(2));
    EXPECT_EQ(listener.top_of_book.size(), 2U);
    EXPECT_TRUE(book.IsReady());
}

TEST(L3Book, CrossedSnapshotReportsIntegrityIssueAndDesyncs) {
    RecordingListener listener;
    L3Book book(listener);

    book.ApplySnapshot(L3Snapshot{{Add(1, Side::kBid, 101, 5), Add(2, Side::kAsk, 100, 3)}});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kCrossedBook);
    EXPECT_EQ(book.GetReadiness(), Readiness::kDesynced);
}

// A depth-limited feed re-announces an order with a fresh Add for the same
// order_id; that must replace the tracked copy rather than duplicate it.
TEST(L3Book, AddOfKnownOrderIdReplacesInsteadOfDuplicating) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{});
    ApplyBatch(book, {Add(1, Side::kBid, 100, 5)});

    ApplyBatch(book, {Add(1, Side::kBid, 100, 7)});

    const BookView view = book.Policy().GetBookView();
    ASSERT_EQ(view.Entries(Side::kBid).size(), 1U);
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(7)}));
    EXPECT_TRUE(listener.integrity_issues.empty());
}

// --- Depth ---

constexpr std::size_t kDepth = 3;

// Adds kDepth + 1 bid levels, one order each, at prices 100, 101, ... Returns
// them as updates; the lowest price (100, order id 1) is the one a book
// retaining kDepth levels has to drop.
std::vector<L3Update> OneMoreBidLevelThanDepth() {
    std::vector<L3Update> updates;
    updates.reserve(kDepth + 1);
    for (std::size_t level = 0; level <= kDepth; ++level) {
        updates.push_back(
            Add(level + 1, Side::kBid, static_cast<std::int64_t>(100 + level), /*quantity=*/1));
    }
    return updates;
}

TEST(L3Book, BatchIsTruncatedToConfiguredDepth) {
    RecordingListener listener;
    L3Book book(listener, L3Policy(kDepth));
    book.ApplySnapshot(L3Snapshot{});

    ApplyBatch(book, OneMoreBidLevelThanDepth());

    const BookView view = book.Policy().GetBookView();
    const std::vector<BookEntry>& bids = view.Entries(Side::kBid);
    ASSERT_EQ(bids.size(), kDepth);
    EXPECT_EQ(bids.front().price, Price(static_cast<std::int64_t>(100 + kDepth)));
    EXPECT_EQ(bids.back().price, Price(101));
    // The dropped order is gone from the index too, not just from its level.
    EXPECT_EQ(book.Policy().OrderQuantity(OrderId(1)), std::nullopt);
}

TEST(L3Book, SnapshotIsTruncatedToConfiguredDepth) {
    RecordingListener listener;
    L3Book book(listener, L3Policy(kDepth));

    book.ApplySnapshot(L3Snapshot{OneMoreBidLevelThanDepth()});

    EXPECT_EQ(book.Policy().Levels(Side::kBid).size(), kDepth);
}

TEST(L3Book, RetainsEveryLevelWithoutADepthLimit) {
    RecordingListener listener;
    L3Book book(listener);
    book.ApplySnapshot(L3Snapshot{});
    constexpr std::size_t kManyLevels = 50;
    std::vector<L3Update> updates;
    updates.reserve(kManyLevels);
    for (std::size_t level = 0; level < kManyLevels; ++level) {
        updates.push_back(Add(level + 1, Side::kBid, static_cast<std::int64_t>(100 + level), 1));
    }

    ApplyBatch(book, updates);

    EXPECT_EQ(book.Policy().Levels(Side::kBid).size(), kManyLevels);
}

}  // namespace
}  // namespace order_book
