#include "book_adapter/journal_replay.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "book_adapter/book_adapter.h"
#include "book_adapter/book_settings.h"
#include "feed_handler/journal_writer.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/test_support.h"
#include "order_book/instrument_scale.h"
#include "order_book/types.h"

namespace book_adapter {
namespace {

using order_book::IntegrityIssue;
using order_book::Readiness;

// Settings of the committed Kraken capture, the ones kraken_capture_replay_test
// documents: BTC/USD reference data gives price decimals 1 and lot decimals 8,
// and it was subscribed at Kraken's default depth 10.
constexpr order_book::InstrumentScale kCaptureScale(/*price_decimals=*/1, /*quantity_decimals=*/8);
constexpr std::size_t kCaptureDepth = 10;

// What kraken_capture_replay_test asserts about the same file: 6496 wire
// messages, of which 1 snapshot and 6361 updates carry a checksum.
constexpr std::uint64_t kFixtureWireMessages = 6496;
constexpr std::uint64_t kFixtureUpdates = 6361;

constexpr std::string_view kHeartbeat = R"({"channel":"heartbeat"})";

std::filesystem::path FixturePath() {
    return std::filesystem::path(TEST_DATA_DIR) / "kraken_l3_capture.journal";
}

ReplayOptions CaptureOptions() {
    return ReplayOptions{.scale = kCaptureScale, .kraken_depth = kCaptureDepth};
}

std::span<const std::byte> BytesOf(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

// A directory of its own under the temp dir, removed on destruction, so a test
// can write journals without leaving anything behind.
class ScopedDir {
  public:
    ScopedDir() : path_(feed_handler::test_support::UniqueTestDir("journal_replay_test")) {
        std::filesystem::create_directories(path_);
    }
    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;
    ScopedDir(ScopedDir&&) = delete;
    ScopedDir& operator=(ScopedDir&&) = delete;
    ~ScopedDir() {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] const std::filesystem::path& Path() const {
        return path_;
    }

    // A journal written with the real writer: a connect marker (unless
    // `with_marker` is false), then `frames`.
    std::filesystem::path WriteJournal(std::string_view name, const std::string& exchange,
                                       const std::vector<std::string_view>& frames,
                                       bool with_marker = true,
                                       std::uint64_t connect_id = 1) const {
        const std::filesystem::path path = path_ / std::string(name);
        feed_handler::CaptureStamper stamper;
        feed_handler::JournalWriter writer(path, {.exchange = exchange, .connect_id = connect_id});
        if (with_marker) {
            writer.WriteConnectMarker(stamper.Stamp(BytesOf("test")));
        }
        for (const std::string_view frame : frames) {
            writer.OnFrame(stamper.Stamp(BytesOf(frame)));
        }
        writer.Flush();
        return path;
    }

  private:
    std::filesystem::path path_;
};

const char* ErrorOf(const std::expected<ReplayReport, std::string>& report) {
    return report.has_value() ? "" : report.error().c_str();
}

TEST(JournalReplay, CommittedKrakenCaptureGivesTheSameNumbersAsTheBookReplayTest) {
    JournalReplay replay(CaptureOptions());
    const std::vector<std::filesystem::path> paths{FixturePath()};
    const auto report = replay.Run(paths);
    ASSERT_TRUE(report.has_value()) << ErrorOf(report);

    EXPECT_FALSE(report->StoppedEarly());
    ASSERT_EQ(report->files.size(), 1U);
    EXPECT_EQ(report->files[0].exchange, "kraken");
    EXPECT_EQ(report->wire_messages, kFixtureWireMessages);
    EXPECT_EQ(report->files[0].wire_messages, kFixtureWireMessages);
    // The extra record is the connect marker.
    EXPECT_EQ(report->files[0].records, kFixtureWireMessages + 1);

    const ConnectionStats& stats = report->stats;
    EXPECT_EQ(stats.frames, kFixtureWireMessages);
    EXPECT_EQ(stats.snapshots, 1U);
    // Exact, not a lower bound: a parser change that silently drops or duplicates
    // messages would pass a "greater than zero" check just as easily.
    EXPECT_EQ(stats.updates, kFixtureUpdates);
    EXPECT_EQ(stats.updates_before_snapshot, 0U);
    EXPECT_EQ(stats.updates_while_desynced, 0U);
    EXPECT_EQ(stats.IssueCount(IntegrityIssue::kChecksumMismatch), 0U);
    EXPECT_EQ(stats.IssueCount(IntegrityIssue::kUnknownOrder), 0U);
    EXPECT_EQ(stats.IssueCount(IntegrityIssue::kCrossedBook), 0U);
    EXPECT_EQ(stats.TotalIssues(), 0U);
    EXPECT_EQ(stats.parse_errors, 0U);
    EXPECT_EQ(stats.apply_errors, 0U);
    EXPECT_EQ(stats.frames_unsupported, 0U);
    EXPECT_EQ(stats.connects, 1U);
    EXPECT_EQ(stats.disconnects, 1U);

    const BookAdapter& adapter = replay.Adapter();
    ASSERT_EQ(adapter.ConnectionCount(), 1U);
    EXPECT_EQ(adapter.BookCount(JournalReplay::kConnection), 1U);
    const KrakenBook* book = adapter.FindKrakenBook(JournalReplay::kConnection, "BTC/USD");
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);
}

TEST(JournalReplay, ParseAndApplyTimeAreReportedSeparatelyFromTheTotal) {
    JournalReplay replay(CaptureOptions());
    const std::vector<std::filesystem::path> paths{FixturePath()};
    const auto report = replay.Run(paths);
    ASSERT_TRUE(report.has_value()) << ErrorOf(report);

    EXPECT_TRUE(report->timed);
    EXPECT_GT(report->stats.parse_ns, 0U);
    EXPECT_GT(report->stats.apply_ns, 0U);
    EXPECT_LE(report->stats.parse_ns + report->stats.apply_ns, report->total_ns);
    EXPECT_EQ(report->ReadNs(), report->total_ns - report->stats.parse_ns - report->stats.apply_ns);
}

TEST(JournalReplay, WithoutTimingNothingIsMeasuredButTheCountsAreTheSame) {
    ReplayOptions options = CaptureOptions();
    options.measure_timing = false;
    JournalReplay replay(options);
    const std::vector<std::filesystem::path> paths{FixturePath()};
    const auto report = replay.Run(paths);
    ASSERT_TRUE(report.has_value()) << ErrorOf(report);

    EXPECT_FALSE(report->timed);
    EXPECT_EQ(report->stats.parse_ns, 0U);
    EXPECT_EQ(report->stats.apply_ns, 0U);
    EXPECT_EQ(report->ReadNs(), 0U);
    EXPECT_GT(report->total_ns, 0U);
    EXPECT_EQ(report->stats.updates, kFixtureUpdates);
    EXPECT_EQ(report->stats.TotalIssues(), 0U);

    std::ostringstream text;
    PrintReport(text, *report);
    EXPECT_NE(text.str().find("not measured"), std::string::npos) << text.str();
}

TEST(JournalReplay, OrderedListReplaysEveryFileAndEachConnectMarkerResetsTheBooks) {
    JournalReplay replay(CaptureOptions());
    const std::vector<std::filesystem::path> paths{FixturePath(), FixturePath()};
    const auto report = replay.Run(paths);
    ASSERT_TRUE(report.has_value()) << ErrorOf(report);

    ASSERT_EQ(report->files.size(), 2U);
    EXPECT_EQ(report->wire_messages, 2 * kFixtureWireMessages);
    EXPECT_EQ(report->stats.snapshots, 2U);
    EXPECT_EQ(report->stats.updates, 2 * kFixtureUpdates);
    // The second file's marker ended the first connect and began a new one; the
    // second snapshot only applies to a fresh book, so no issue is counted.
    EXPECT_EQ(report->stats.connects, 2U);
    EXPECT_EQ(report->stats.disconnects, 2U);
    EXPECT_EQ(report->stats.TotalIssues(), 0U);
    const KrakenBook* book = replay.Adapter().FindKrakenBook(JournalReplay::kConnection, "BTC/USD");
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);
}

TEST(JournalReplay, ContinuationFileWithoutAConnectRecordDoesNotStaleTheConnection) {
    const ScopedDir dir;
    const std::vector<std::filesystem::path> paths{
        dir.WriteJournal("first.journal", "kraken", {kHeartbeat, kHeartbeat}),
        dir.WriteJournal("second.journal", "kraken", {kHeartbeat}, /*with_marker=*/false),
    };
    JournalReplay replay(CaptureOptions());
    const auto report = replay.Run(paths);
    ASSERT_TRUE(report.has_value()) << ErrorOf(report);

    EXPECT_EQ(report->wire_messages, 3U);
    EXPECT_EQ(report->stats.frames, 3U);
    EXPECT_EQ(report->stats.connects, 1U);
    // Only the end of the whole replay is a disconnect, not the file boundary.
    EXPECT_EQ(report->stats.disconnects, 1U);
    EXPECT_EQ(report->stats.frames_ignored_stale, 0U);
}

TEST(JournalReplay, MalformedPayloadIsCountedNotFatal) {
    const ScopedDir dir;
    const std::vector<std::filesystem::path> paths{
        dir.WriteJournal("bad.journal", "kraken", {"this is not json", kHeartbeat}),
    };
    JournalReplay replay(CaptureOptions());
    const auto report = replay.Run(paths);
    ASSERT_TRUE(report.has_value()) << ErrorOf(report);

    EXPECT_EQ(report->stats.frames, 2U);
    EXPECT_EQ(report->stats.parse_errors, 1U);
    EXPECT_EQ(report->stats.snapshots, 0U);
    EXPECT_EQ(report->stats.updates, 0U);
}

TEST(JournalReplay, TruncatedJournalIsReportedNotIgnored) {
    const ScopedDir dir;
    // The fixture minus the last bytes: a crash mid-write of the final record.
    std::ifstream input(FixturePath(), std::ios::binary);
    ASSERT_TRUE(input.good()) << "missing capture fixture: " << FixturePath();
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    constexpr std::size_t kCutBytes = 5;
    ASSERT_GT(bytes.size(), kCutBytes);
    const std::filesystem::path cut = dir.Path() / "cut.journal";
    {
        std::ofstream out(cut, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() - kCutBytes));
        ASSERT_TRUE(out.good());
    }

    JournalReplay replay(CaptureOptions());
    const std::vector<std::filesystem::path> paths{cut};
    const auto report = replay.Run(paths);
    ASSERT_TRUE(report.has_value()) << ErrorOf(report);

    EXPECT_TRUE(report->StoppedEarly());
    ASSERT_EQ(report->files.size(), 1U);
    EXPECT_TRUE(report->files[0].stopped_early);
    EXPECT_FALSE(report->files[0].stop_reason.empty());
    // Everything before the torn record was replayed, the torn one was not.
    EXPECT_EQ(report->wire_messages, kFixtureWireMessages - 1);

    std::ostringstream text;
    PrintReport(text, *report);
    EXPECT_NE(text.str().find("STOPPED EARLY"), std::string::npos) << text.str();
}

TEST(JournalReplay, ReportTextCarriesTheCounts) {
    JournalReplay replay(CaptureOptions());
    const std::vector<std::filesystem::path> paths{FixturePath()};
    const auto report = replay.Run(paths);
    ASSERT_TRUE(report.has_value()) << ErrorOf(report);

    std::ostringstream text;
    PrintReport(text, *report);
    EXPECT_NE(text.str().find("wire messages: 6496"), std::string::npos) << text.str();
    EXPECT_NE(text.str().find("snapshots: 1"), std::string::npos) << text.str();
    EXPECT_NE(text.str().find("updates: 6361"), std::string::npos) << text.str();
    EXPECT_NE(text.str().find("integrity issues: 0"), std::string::npos) << text.str();
    EXPECT_NE(text.str().find("parse time:"), std::string::npos) << text.str();
    EXPECT_NE(text.str().find("apply time:"), std::string::npos) << text.str();
    EXPECT_EQ(text.str().find("STOPPED EARLY"), std::string::npos) << text.str();
}

TEST(JournalReplay, EmptyListFails) {
    JournalReplay replay(CaptureOptions());
    const auto report = replay.Run(std::span<const std::filesystem::path>{});
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().find("no journal"), std::string::npos) << report.error();
}

TEST(JournalReplay, MissingFileFailsAndNamesIt) {
    const ScopedDir dir;
    const std::filesystem::path missing = dir.Path() / "nope.journal";
    JournalReplay replay(CaptureOptions());
    const std::vector<std::filesystem::path> paths{missing};
    const auto report = replay.Run(paths);
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().find("nope.journal"), std::string::npos) << report.error();
}

TEST(JournalReplay, ExchangeTagWithoutABookFails) {
    const ScopedDir dir;
    const std::vector<std::filesystem::path> paths{
        dir.WriteJournal("odd.journal", "bogus", {kHeartbeat}),
    };
    JournalReplay replay(CaptureOptions());
    const auto report = replay.Run(paths);
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().find("bogus"), std::string::npos) << report.error();
}

TEST(JournalReplay, MixingExchangesInOneRunFails) {
    const ScopedDir dir;
    const std::vector<std::filesystem::path> paths{
        dir.WriteJournal("kraken.journal", "kraken", {kHeartbeat}),
        dir.WriteJournal("deribit.journal", "deribit", {kHeartbeat}),
    };
    JournalReplay replay(CaptureOptions());
    const auto report = replay.Run(paths);
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().find("deribit.journal"), std::string::npos) << report.error();
}

}  // namespace
}  // namespace book_adapter
