#include "order_book/kraken_l3_policy.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "order_book/engine.h"
#include "order_book/kraken_checksum.h"
#include "order_book/tests/kraken_documented_example.h"
#include "order_book/tests/l3_test_util.h"
#include "order_book/types.h"

namespace order_book {
namespace {

struct RecordingListener {
    std::vector<IntegrityIssue> integrity_issues;

    void OnTopOfBookChanged(Side, std::optional<BookEntry>) {}
    void OnIntegrityCheckFailed(IntegrityIssue issue) {
        integrity_issues.push_back(issue);
    }
};

using KrakenBook = OrderBook<KrakenL3Policy, RecordingListener>;

// These tests populate a handful of levels at most, so any depth comfortably
// above that will do; it has no significance beyond that.
constexpr std::size_t kAmpleDepth = 100;

KrakenL3Update Add(std::uint64_t order_id, Side side, std::int64_t price, std::int64_t quantity,
                   const std::string& timestamp) {
    return KrakenL3Update{l3_test::Add(order_id, side, price, quantity), timestamp};
}

// Kraken's checksum for whatever state applying `updates` produces, computed on
// a scratch copy. This is deliberately not an independent oracle -- it only lets
// tests that are about something other than the checksum's value supply a
// correct one. The documented-example test below is the independent check.
ChecksumMeta ChecksumAfter(const KrakenL3Policy& policy, std::span<const KrakenL3Update> updates) {
    KrakenL3Policy scratch = policy;
    scratch.ApplyBatch(updates, ChecksumMeta{0});
    return ChecksumMeta{KrakenL3Checksum(scratch.Book().GetBookView())};
}

void ApplyBatch(KrakenBook& book, const std::vector<KrakenL3Update>& updates) {
    const std::span<const KrakenL3Update> view(updates);
    book.ApplyBatch(view, ChecksumAfter(book.Policy(), view));
}

const std::string kEarlier = "2026-09-17T00:00:01.000000000Z";
const std::string kLater = "2026-09-17T00:00:02.000000000Z";

// Kraken's own documented value: the one independent check that the
// checksum, the book's per-order queue ordering and the top-10 window agree
// with Kraken's, rather than with our own code.
TEST(KrakenL3Policy, SnapshotMatchingKrakensDocumentedChecksumIsAccepted) {
    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kKrakenDocumentedLevelsPerSide));

    book.ApplySnapshot(L3Snapshot{KrakenDocumentedExampleOrders()},
                       ChecksumMeta{kKrakenDocumentedChecksum});

    EXPECT_TRUE(listener.integrity_issues.empty());
    EXPECT_EQ(book.GetReadiness(), Readiness::kReady);
}

TEST(KrakenL3Policy, SnapshotWithWrongChecksumReportsMismatchAndDesyncs) {
    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kKrakenDocumentedLevelsPerSide));

    book.ApplySnapshot(L3Snapshot{KrakenDocumentedExampleOrders()},
                       ChecksumMeta{kKrakenDocumentedChecksum + 1});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kChecksumMismatch);
    EXPECT_EQ(book.GetReadiness(), Readiness::kDesynced);
}

TEST(KrakenL3Policy, BatchWithCorrectChecksumStaysReady) {
    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kAmpleDepth));
    book.ApplySnapshot(L3Snapshot{}, ChecksumMeta{0});

    ApplyBatch(book, {Add(1, Side::kBid, 100, 5, kEarlier)});

    EXPECT_TRUE(listener.integrity_issues.empty());
    EXPECT_EQ(book.GetReadiness(), Readiness::kReady);
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(5)}));
}

TEST(KrakenL3Policy, BatchWithWrongChecksumReportsMismatchAndDesyncs) {
    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kAmpleDepth));
    book.ApplySnapshot(L3Snapshot{}, ChecksumMeta{0});

    const std::vector<KrakenL3Update> updates{Add(1, Side::kBid, 100, 5, kEarlier)};
    book.ApplyBatch(std::span<const KrakenL3Update>(updates), ChecksumMeta{0xDEADBEEFU});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kChecksumMismatch);
    EXPECT_EQ(book.GetReadiness(), Readiness::kDesynced);
}

// An earlier, more specific issue is not overwritten by the checksum mismatch
// it necessarily causes.
TEST(KrakenL3Policy, UnknownOrderIsReportedInsteadOfAChecksumMismatch) {
    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kAmpleDepth));
    book.ApplySnapshot(L3Snapshot{}, ChecksumMeta{0});

    const std::vector<KrakenL3Update> updates{KrakenL3Update{l3_test::Delete(999), kEarlier}};
    book.ApplyBatch(std::span<const KrakenL3Update>(updates), ChecksumMeta{0xDEADBEEFU});

    ASSERT_EQ(listener.integrity_issues.size(), 1U);
    EXPECT_EQ(listener.integrity_issues[0], IntegrityIssue::kUnknownOrder);
}

// --- Add ordering ---

// Queue priority (and so the checksum) depends on true arrival order, which
// Kraken's JSON array order does not always match; the Adds are ordered by
// their own timestamps instead (exchanges/kraken.md).
TEST(KrakenL3Policy, AddOrderingUsesTimestampNotArrayPosition) {
    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kAmpleDepth));
    book.ApplySnapshot(L3Snapshot{}, ChecksumMeta{0});

    ApplyBatch(book, {Add(2, Side::kBid, 100, 3, kLater), Add(1, Side::kBid, 100, 5, kEarlier)});

    const BookView view = book.Policy().Book().GetBookView();
    const std::vector<BookEntry>& bids = view.Entries(Side::kBid);
    ASSERT_EQ(bids.size(), 2U);
    EXPECT_EQ(bids[0], (BookEntry{Price(100), Quantity(5)}));
    EXPECT_EQ(bids[1], (BookEntry{Price(100), Quantity(3)}));
    EXPECT_TRUE(listener.integrity_issues.empty());
}

// One message can carry an Add followed by a Modify of that same order. Only
// the Adds are reordered, in place, so the Modify still lands after its Add.
TEST(KrakenL3Policy, AddFollowedByModifyOfSameOrderInOneBatchPreservesOrder) {
    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kAmpleDepth));
    book.ApplySnapshot(L3Snapshot{}, ChecksumMeta{0});

    ApplyBatch(book, {Add(1, Side::kBid, 100, 5, kEarlier),
                      KrakenL3Update{l3_test::Modify(1, 9), kLater}});

    EXPECT_TRUE(listener.integrity_issues.empty());
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(100), Quantity(9)}));
}

// Adds sort among themselves into the slots Adds originally occupied, with a
// non-Add event between them still applied at its own position.
TEST(KrakenL3Policy, AddsSortAroundANonAddBetweenThem) {
    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kAmpleDepth));
    book.ApplySnapshot(L3Snapshot{}, ChecksumMeta{0});
    ApplyBatch(book, {Add(1, Side::kBid, 100, 5, kEarlier)});

    // [Add later, Delete 1, Add earlier]: applied as [Add earlier, Delete 1,
    // Add later], so order 3 (earlier) precedes order 2 (later) in the queue.
    ApplyBatch(book,
               {Add(2, Side::kBid, 100, 2, kLater), KrakenL3Update{l3_test::Delete(1), kLater},
                Add(3, Side::kBid, 100, 3, kEarlier)});

    const BookView view = book.Policy().Book().GetBookView();
    const std::vector<BookEntry>& bids = view.Entries(Side::kBid);
    ASSERT_EQ(bids.size(), 2U);
    EXPECT_EQ(bids[0], (BookEntry{Price(100), Quantity(3)}));
    EXPECT_EQ(bids[1], (BookEntry{Price(100), Quantity(2)}));
    EXPECT_TRUE(listener.integrity_issues.empty());
}

// --- Depth ---

// The subscribed depth is handed to the generic book, which drops the levels
// Kraken never sends a delete for.
TEST(KrakenL3Policy, SubscribedDepthLimitsRetainedLevels) {
    constexpr std::size_t kSubscribedDepth = 2;
    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kSubscribedDepth));
    book.ApplySnapshot(L3Snapshot{}, ChecksumMeta{0});

    ApplyBatch(book, {Add(1, Side::kBid, 100, 1, kEarlier), Add(2, Side::kBid, 101, 1, kEarlier),
                      Add(3, Side::kBid, 102, 1, kLater)});

    EXPECT_EQ(book.Policy().Book().Levels(Side::kBid).size(), kSubscribedDepth);
    EXPECT_EQ(book.Best(Side::kBid), (BookEntry{Price(102), Quantity(1)}));
}

}  // namespace
}  // namespace order_book
