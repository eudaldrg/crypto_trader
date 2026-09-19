#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <vector>

#include "order_book/engine.h"
#include "order_book/l2_policy.h"
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
    std::vector<IntegrityIssue> integrity_issues;

    void OnTopOfBookChanged(Side side, std::optional<BookEntry> best) {
        top_of_book.push_back({side, best});
    }
    void OnLevelChanged(Side side, Price price, Quantity quantity) {
        level_changes.push_back({side, price, quantity});
    }
    void OnLevelDeleted(Side side, Price price) {
        level_changes.push_back({side, price, std::nullopt});
    }
    void OnIntegrityCheckFailed(IntegrityIssue issue) {
        integrity_issues.push_back(issue);
    }
};

using L2Book = OrderBook<L2Policy, RecordingListener>;

// Applies one batch: change_id is the message's own id, prev_change_id the id it
// claims to follow.
void ApplyChange(L2Book& book, ChangeId change_id, ChangeId prev_change_id,
                 const std::vector<L2Update>& updates) {
    book.ApplyBatch(std::span<const L2Update>(updates), ChangeIdMeta{change_id, prev_change_id});
}

L2Snapshot MakeSnapshot(std::vector<L2Update> levels, ChangeId change_id) {
    return L2Snapshot{.levels = std::move(levels), .change_id = change_id};
}

TEST(L2Book, AddsNewLevel) {
    RecordingListener listener;
    L2Book book(listener);
    book.ApplySnapshot(MakeSnapshot({}, ChangeId(1)));
    listener.top_of_book.clear();

    ApplyChange(book, ChangeId(2), ChangeId(1),
                {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew}});

    ASSERT_EQ(listener.level_changes.size(), 1U);
    EXPECT_EQ(listener.level_changes[0].side, Side::kBid);
    EXPECT_EQ(listener.level_changes[0].price, Price(100));
    EXPECT_EQ(listener.level_changes[0].quantity, Quantity(5));
    ASSERT_EQ(listener.top_of_book.size(), 1U);
    EXPECT_EQ(listener.top_of_book[0].side, Side::kBid);
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(5)}));
    EXPECT_TRUE(listener.integrity_issues.empty());
}

TEST(L2Book, ModifiesExistingLevelQuantity) {
    RecordingListener listener;
    L2Book book(listener);
    book.ApplySnapshot(MakeSnapshot(
        {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew}}, ChangeId(1)));
    listener.level_changes.clear();

    ApplyChange(book, ChangeId(2), ChangeId(1),
                {L2Update{Side::kBid, Price(100), Quantity(9), L2Operation::kChange}});

    ASSERT_EQ(listener.level_changes.size(), 1U);
    EXPECT_EQ(listener.level_changes[0].quantity, Quantity(9));
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(9)}));
}

TEST(L2Book, DeletesLevelViaDeleteOperation) {
    RecordingListener listener;
    L2Book book(listener);
    book.ApplySnapshot(MakeSnapshot(
        {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew}}, ChangeId(1)));
    listener.level_changes.clear();
    listener.top_of_book.clear();

    ApplyChange(book, ChangeId(2), ChangeId(1),
                {L2Update{Side::kBid, Price(100), Quantity(0), L2Operation::kDelete}});

    ASSERT_EQ(listener.level_changes.size(), 1U);
    EXPECT_EQ(listener.level_changes[0].quantity, std::nullopt);
    ASSERT_EQ(listener.top_of_book.size(), 1U);
    EXPECT_EQ(listener.top_of_book[0].best, std::nullopt);
    EXPECT_EQ(book.Best(Side::kBid), std::nullopt);
}

TEST(L2Book, TopOfBookOnlyFiresForBestLevelChange) {
    RecordingListener listener;
    L2Book book(listener);
    book.ApplySnapshot(
        MakeSnapshot({L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew},
                      L2Update{Side::kBid, Price(99), Quantity(3), L2Operation::kNew}},
                     ChangeId(1)));
    listener.top_of_book.clear();
    listener.level_changes.clear();

    // Touch only the second-best level (99), not the best (100).
    ApplyChange(book, ChangeId(2), ChangeId(1),
                {L2Update{Side::kBid, Price(99), Quantity(7), L2Operation::kChange}});

    EXPECT_EQ(listener.level_changes.size(), 1U);
    EXPECT_TRUE(listener.top_of_book.empty());
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(5)}));
}

TEST(L2Book, ChangeIdGapReportsIntegrityIssueAndDoesNotMutate) {
    RecordingListener listener;
    L2Book book(listener);
    book.ApplySnapshot(MakeSnapshot(
        {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew}}, ChangeId(1)));
    listener.level_changes.clear();
    listener.top_of_book.clear();

    // prev_change_id (5) does not match the last applied change_id (1).
    ApplyChange(book, ChangeId(6), ChangeId(5),
                {L2Update{Side::kBid, Price(100), Quantity(9), L2Operation::kChange}});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kGap);
    EXPECT_TRUE(listener.level_changes.empty());
    EXPECT_TRUE(listener.top_of_book.empty());
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(5)}));
    EXPECT_EQ(book.GetReadiness(), Readiness::kDesynced);
}

TEST(L2Book, ApplyBatchOnDesyncedBookIsANoOp) {
    RecordingListener listener;
    L2Book book(listener);
    book.ApplySnapshot(MakeSnapshot(
        {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew}}, ChangeId(1)));

    // Force a desync.
    ApplyChange(book, ChangeId(6), ChangeId(5),
                {L2Update{Side::kBid, Price(100), Quantity(9), L2Operation::kChange}});
    ASSERT_EQ(book.GetReadiness(), Readiness::kDesynced);
    listener.integrity_issues.clear();
    listener.level_changes.clear();

    ApplyChange(book, ChangeId(7), ChangeId(6),
                {L2Update{Side::kBid, Price(101), Quantity(1), L2Operation::kNew}});

    EXPECT_TRUE(listener.integrity_issues.empty());
    EXPECT_TRUE(listener.level_changes.empty());
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(5)}));
}

TEST(L2Book, CrossedBookAfterUpdateReportsIntegrityIssue) {
    RecordingListener listener;
    L2Book book(listener);
    book.ApplySnapshot(
        MakeSnapshot({L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew},
                      L2Update{Side::kAsk, Price(101), Quantity(5), L2Operation::kNew}},
                     ChangeId(1)));
    listener.integrity_issues.clear();

    ApplyChange(book, ChangeId(2), ChangeId(1),
                {L2Update{Side::kBid, Price(102), Quantity(5), L2Operation::kNew}});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kCrossedBook);
}

TEST(L2Book, DeleteOfUnknownPriceReportsUnknownLevel) {
    RecordingListener listener;
    L2Book book(listener);
    book.ApplySnapshot(MakeSnapshot({}, ChangeId(1)));
    listener.integrity_issues.clear();

    ApplyChange(book, ChangeId(2), ChangeId(1),
                {L2Update{Side::kBid, Price(100), Quantity(0), L2Operation::kDelete}});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kUnknownLevel);
    EXPECT_TRUE(listener.level_changes.empty());
}

}  // namespace
}  // namespace order_book
