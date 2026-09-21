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

// L2 without a change_id (FIX-fed books, decisions/0006, "L2 without a
// change_id"): the same level rules with no gap check and no meta anywhere.
using UnsequencedBook = OrderBook<UnsequencedL2Policy, RecordingListener>;

void ApplyUnsequenced(UnsequencedBook& book, const std::vector<L2Update>& updates) {
    book.ApplyBatch(std::span<const L2Update>(updates));
}

UnsequencedL2Snapshot MakeUnsequencedSnapshot(std::vector<L2Update> levels) {
    return UnsequencedL2Snapshot{.levels = std::move(levels)};
}

TEST(UnsequencedL2Book, SnapshotNeedsNoChangeIdAndBecomesReady) {
    RecordingListener listener;
    UnsequencedBook book(listener);
    EXPECT_EQ(book.GetReadiness(), Readiness::kUninitialized);

    book.ApplySnapshot(MakeUnsequencedSnapshot(
        {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew},
         L2Update{Side::kAsk, Price(101), Quantity(4), L2Operation::kNew}}));

    EXPECT_TRUE(book.IsReady());
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(5)}));
    EXPECT_EQ(book.Best(Side::kAsk), (BookEntry{Price(101), Quantity(4)}));
    EXPECT_EQ(listener.level_changes.size(), 2U);
    EXPECT_TRUE(listener.integrity_issues.empty());
}

TEST(UnsequencedL2Book, AppliesNewChangeAndDelete) {
    RecordingListener listener;
    UnsequencedBook book(listener);
    book.ApplySnapshot(MakeUnsequencedSnapshot({}));

    ApplyUnsequenced(book, {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew}});
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(5)}));

    ApplyUnsequenced(book, {L2Update{Side::kBid, Price(100), Quantity(9), L2Operation::kChange}});
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(9)}));

    listener.top_of_book.clear();
    ApplyUnsequenced(book, {L2Update{Side::kBid, Price(100), Quantity(0), L2Operation::kDelete}});
    EXPECT_EQ(book.Best(Side::kBid), std::nullopt);
    ASSERT_EQ(listener.top_of_book.size(), 1U);
    EXPECT_EQ(listener.top_of_book[0].best, std::nullopt);

    ASSERT_EQ(listener.level_changes.size(), 3U);
    EXPECT_EQ(listener.level_changes[0].quantity, Quantity(5));
    EXPECT_EQ(listener.level_changes[1].quantity, Quantity(9));
    EXPECT_EQ(listener.level_changes[2].quantity, std::nullopt);
    EXPECT_TRUE(listener.integrity_issues.empty());
    EXPECT_TRUE(book.IsReady());
}

TEST(UnsequencedL2Book, BatchesCarryNoGapCheck) {
    RecordingListener listener;
    UnsequencedBook book(listener);
    book.ApplySnapshot(MakeUnsequencedSnapshot({}));

    // Any number of consecutive batches apply; there is no id to disagree.
    for (std::int64_t i = 1; i <= 3; ++i) {
        ApplyUnsequenced(book,
                         {L2Update{Side::kBid, Price(100 + i), Quantity(i), L2Operation::kNew}});
    }

    EXPECT_TRUE(listener.integrity_issues.empty());
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(103), Quantity(3)}));
}

TEST(UnsequencedL2Book, ChangeOnUnknownLevelReportsUnknownLevel) {
    RecordingListener listener;
    UnsequencedBook book(listener);
    book.ApplySnapshot(MakeUnsequencedSnapshot({}));
    listener.level_changes.clear();

    ApplyUnsequenced(book, {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kChange}});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kUnknownLevel);
    EXPECT_TRUE(listener.level_changes.empty());
    EXPECT_EQ(book.GetReadiness(), Readiness::kDesynced);
}

TEST(UnsequencedL2Book, DeleteOfUnknownLevelReportsUnknownLevel) {
    RecordingListener listener;
    UnsequencedBook book(listener);
    book.ApplySnapshot(MakeUnsequencedSnapshot({}));

    ApplyUnsequenced(book, {L2Update{Side::kAsk, Price(100), Quantity(0), L2Operation::kDelete}});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kUnknownLevel);
}

TEST(UnsequencedL2Book, CrossedBatchReportsCrossedBook) {
    RecordingListener listener;
    UnsequencedBook book(listener);
    book.ApplySnapshot(MakeUnsequencedSnapshot(
        {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kNew},
         L2Update{Side::kAsk, Price(101), Quantity(5), L2Operation::kNew}}));
    listener.integrity_issues.clear();

    ApplyUnsequenced(book, {L2Update{Side::kBid, Price(102), Quantity(5), L2Operation::kNew}});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kCrossedBook);
    EXPECT_EQ(book.GetReadiness(), Readiness::kDesynced);
}

TEST(UnsequencedL2Book, CrossedSnapshotReportsCrossedBook) {
    RecordingListener listener;
    UnsequencedBook book(listener);

    book.ApplySnapshot(MakeUnsequencedSnapshot(
        {L2Update{Side::kBid, Price(102), Quantity(5), L2Operation::kNew},
         L2Update{Side::kAsk, Price(101), Quantity(5), L2Operation::kNew}}));

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kCrossedBook);
    EXPECT_EQ(book.GetReadiness(), Readiness::kDesynced);
}

TEST(UnsequencedL2Book, BatchOnDesyncedBookIsANoOp) {
    RecordingListener listener;
    UnsequencedBook book(listener);
    book.ApplySnapshot(MakeUnsequencedSnapshot({}));
    ApplyUnsequenced(book, {L2Update{Side::kBid, Price(100), Quantity(5), L2Operation::kChange}});
    ASSERT_EQ(book.GetReadiness(), Readiness::kDesynced);
    listener.integrity_issues.clear();

    ApplyUnsequenced(book, {L2Update{Side::kBid, Price(101), Quantity(1), L2Operation::kNew}});

    EXPECT_TRUE(listener.integrity_issues.empty());
    EXPECT_EQ(book.Best(Side::kBid), std::nullopt);
}

// The two forms cannot be mixed up: a sequenced book cannot be applied without
// its meta or handed an unsequenced snapshot, and an unsequenced book cannot be
// handed a meta or a sequenced snapshot. Checked on the engine, which is what
// callers use, and on each policy.
template <typename Book, typename... Args>
concept CanApplyBatch = requires(Book& book, std::span<const L2Update> updates,
                                 const Args&... args) { book.ApplyBatch(updates, args...); };

template <typename Book, typename Snapshot>
concept CanApplySnapshot =
    requires(Book& book, const Snapshot& snapshot) { book.ApplySnapshot(snapshot); };

TEST(UnsequencedL2Book, WrongFormDoesNotCompileOnTheEngine) {
    static_assert(CanApplyBatch<L2Book, ChangeIdMeta>);
    static_assert(!CanApplyBatch<L2Book>, "a sequenced book must not apply a batch without a meta");
    static_assert(CanApplyBatch<UnsequencedBook>);
    static_assert(!CanApplyBatch<UnsequencedBook, ChangeIdMeta>,
                  "an unsequenced book must not be handed a change_id");

    static_assert(CanApplySnapshot<L2Book, L2Snapshot>);
    static_assert(!CanApplySnapshot<L2Book, UnsequencedL2Snapshot>);
    static_assert(CanApplySnapshot<UnsequencedBook, UnsequencedL2Snapshot>);
    static_assert(!CanApplySnapshot<UnsequencedBook, L2Snapshot>,
                  "an unsequenced book takes no change_id baseline");
    SUCCEED();
}

TEST(UnsequencedL2Book, WrongFormDoesNotCompileOnThePolicy) {
    static_assert(CanApplyBatch<L2Policy, ChangeIdMeta>);
    static_assert(!CanApplyBatch<L2Policy>);
    static_assert(CanApplyBatch<UnsequencedL2Policy>);
    static_assert(!CanApplyBatch<UnsequencedL2Policy, ChangeIdMeta>);
    SUCCEED();
}

}  // namespace
}  // namespace order_book
