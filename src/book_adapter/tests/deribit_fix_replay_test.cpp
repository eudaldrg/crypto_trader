// Replays a real Deribit FIX capture through the journal replay driver and the
// book adapter, end to end: journal reader, FIX framing already done by the
// client, T13's normalizer, UnsequencedL2Policy. The synthetic FIX messages in
// deribit_fix_book_test.cpp are written from the docs; this is what the wire
// really sends, which is what caught the Deribit WS shape ADR 0006's first
// draft got wrong.
//
// The fixture, deribit_fix_capture.journal, is the first 800 35=X of the hour
// recorded on the Deribit testnet on 2026-09-19, cut by the journal_slice tool
// (src/book_adapter/tools/journal_slice.cpp) and committed compressed in
// src/order_book/tests/data/capture_fixtures.tar.xz. journal_slice drops the
// Logon: Deribit's inbound 35=A carries RawData (96) and Password (554). Never
// print the fixture or a payload from it; every check here is a count.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "book_adapter/book_adapter.h"
#include "book_adapter/journal_replay.h"
#include "feed_handler/journal_format.h"
#include "feed_handler/journal_reader.h"
#include "order_book/instrument_scale.h"
#include "order_book/types.h"

namespace book_adapter {
namespace {

using order_book::IntegrityIssue;
using order_book::Readiness;
using order_book::Side;

// Where the two decimals come from. Price: every 270 (Px) in the capture ends in
// .0 or .5 (checked by counting the digit after the point over all 75470
// prices), which is BTC-PERPETUAL's 0.5 tick size, so one decimal is exact.
// Quantity: every 271 (Size) is a whole number written with a trailing ".0"
// (again counted over all 75470 sizes; Deribit sizes BTC-PERPETUAL in whole USD
// contracts), so no quantity decimals are needed. These are also the values
// config/feed_handler.toml documents for BTC-PERPETUAL.
constexpr order_book::InstrumentScale kScale(/*price_decimals=*/1, /*quantity_decimals=*/0);

// Read off the slice by the journal_slice tool's own raw byte scan (its printed
// "35=W" and "35=X" counts), and cross-checked below against a second raw scan
// in this file. They are NOT taken from the adapter under test.
constexpr std::uint64_t kSnapshots = 1;
constexpr std::uint64_t kIncrementals = 800;
// The two heartbeats (35=0) the slice also holds: session traffic, no book.
constexpr std::uint64_t kOtherMessages = 2;
constexpr std::uint64_t kWireMessages = kSnapshots + kIncrementals + kOtherMessages;

constexpr std::string_view kSymbol = "BTC-PERPETUAL";

// SOH-delimited, so 35= cannot match inside 135= and 96= not inside 1096=.
constexpr std::string_view kLogon =
    "\x01"
    "35=A\x01";
constexpr std::string_view kSnapshotType =
    "\x01"
    "35=W\x01";
constexpr std::string_view kIncrementalType =
    "\x01"
    "35=X\x01";
constexpr std::string_view kRawData =
    "\x01"
    "96=";
constexpr std::string_view kPassword =
    "\x01"
    "554=";

std::filesystem::path FixturePath() {
    return std::filesystem::path(TEST_DATA_DIR) / "deribit_fix_capture.journal";
}

std::uint64_t CountOccurrences(std::string_view text, std::string_view pattern) {
    std::uint64_t count = 0;
    for (std::size_t at = text.find(pattern); at != std::string_view::npos;
         at = text.find(pattern, at + pattern.size())) {
        ++count;
    }
    return count;
}

// The payload as text, copied byte for byte, for the raw scans below.
std::string AsText(std::span<const std::byte> bytes) {
    std::string text(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(),
                           [](std::byte byte) { return static_cast<char>(byte); });
    return text;
}

// Raw counts over every wire message of the fixture, by scanning bytes: no FIX
// parser, no adapter.
struct RawCounts {
    std::uint64_t wire_messages = 0;
    std::uint64_t connect_records = 0;
    std::uint64_t logons = 0;
    std::uint64_t raw_data_fields = 0;
    std::uint64_t password_fields = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t incrementals = 0;
};

RawCounts CountFixture() {
    RawCounts counts;
    auto reader = feed_handler::JournalReader::Open(FixturePath());
    EXPECT_TRUE(reader.has_value()) << "missing capture fixture: " << FixturePath();
    if (!reader) {
        return counts;
    }
    while (const std::optional<feed_handler::JournalRecord> record = reader->Next()) {
        if (record->type == feed_handler::journal::RecordType::kConnect) {
            ++counts.connect_records;
            continue;
        }
        ++counts.wire_messages;
        const std::string text = AsText(record->payload);
        counts.logons += CountOccurrences(text, kLogon);
        counts.raw_data_fields += CountOccurrences(text, kRawData);
        counts.password_fields += CountOccurrences(text, kPassword);
        counts.snapshots += CountOccurrences(text, kSnapshotType);
        counts.incrementals += CountOccurrences(text, kIncrementalType);
    }
    EXPECT_FALSE(reader->StoppedEarly()) << "fixture journal is corrupt or truncated";
    return counts;
}

TEST(DeribitFixReplay, FixtureHoldsNoLogonAndNoCredentialFields) {
    const RawCounts counts = CountFixture();
    EXPECT_EQ(counts.logons, 0U) << "the fixture must not contain a 35=A Logon";
    EXPECT_EQ(counts.raw_data_fields, 0U) << "the fixture must not contain RawData (96)";
    EXPECT_EQ(counts.password_fields, 0U) << "the fixture must not contain Password (554)";
}

TEST(DeribitFixReplay, FixtureCountsMatchWhatTheSliceToolReported) {
    const RawCounts counts = CountFixture();
    EXPECT_EQ(counts.connect_records, 1U);
    EXPECT_EQ(counts.wire_messages, kWireMessages);
    EXPECT_EQ(counts.snapshots, kSnapshots);
    EXPECT_EQ(counts.incrementals, kIncrementals);
}

TEST(DeribitFixReplay, RealCaptureReplaysCleanAndTheBookIsReady) {
    JournalReplay replay(ReplayOptions{.scale = kScale});
    const std::vector<std::filesystem::path> paths{FixturePath()};
    const auto report = replay.Run(paths);
    ASSERT_TRUE(report.has_value()) << report.error();

    EXPECT_FALSE(report->StoppedEarly());
    ASSERT_EQ(report->files.size(), 1U);
    EXPECT_EQ(report->files[0].exchange, "deribit");
    EXPECT_EQ(report->wire_messages, kWireMessages);
    // The extra record is the connect marker.
    EXPECT_EQ(report->files[0].records, kWireMessages + 1);

    const ConnectionStats& stats = report->stats;
    EXPECT_EQ(stats.frames, kWireMessages);
    // Exact, from the raw scan: each 35=W is a snapshot, each 35=X one update.
    EXPECT_EQ(stats.snapshots, kSnapshots);
    EXPECT_EQ(stats.updates, kIncrementals);
    EXPECT_EQ(stats.updates_before_snapshot, 0U);
    EXPECT_EQ(stats.updates_while_desynced, 0U);
    EXPECT_EQ(stats.TotalIssues(), 0U);
    EXPECT_EQ(stats.IssueCount(IntegrityIssue::kUnknownLevel), 0U);
    EXPECT_EQ(stats.IssueCount(IntegrityIssue::kCrossedBook), 0U);
    EXPECT_EQ(stats.parse_errors, 0U);
    EXPECT_EQ(stats.apply_errors, 0U);
    EXPECT_EQ(stats.frames_unsupported, 0U);
    EXPECT_EQ(stats.frames_ignored_stale, 0U);
    EXPECT_EQ(stats.connects, 1U);
    EXPECT_EQ(stats.disconnects, 1U);

    const BookAdapter& adapter = replay.Adapter();
    ASSERT_EQ(adapter.ConnectionCount(), 1U);
    EXPECT_EQ(adapter.BookCount(JournalReplay::kConnection), 1U);
    const DeribitBook* book = adapter.FindDeribitBook(JournalReplay::kConnection, kSymbol);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);

    // A real book after 800 updates has both sides and does not cross.
    const auto bid = book->Best(Side::kBid);
    const auto ask = book->Best(Side::kAsk);
    if (!bid.has_value() || !ask.has_value()) {
        FAIL() << "a side of the book is empty";
    }
    EXPECT_LT(kScale.FromPrice(bid->price), kScale.FromPrice(ask->price));
    EXPECT_GT(bid->quantity.Lots(), 0);
    EXPECT_GT(ask->quantity.Lots(), 0);
}

}  // namespace
}  // namespace book_adapter
