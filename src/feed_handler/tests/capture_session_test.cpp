// Incarnation/journal-rotation bookkeeping: the part of the reconnect path
// that can be tested without a socket. decisions/0004 requires one file per
// (exchange, connection-incarnation), an explicit marker record at the start
// of each, and per-incarnation capture sequence numbers.
#include "feed_handler/capture_session.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

#include "feed_handler/journal_reader.h"

namespace {

using feed_handler::capture_session;
using feed_handler::journal_reader;
using feed_handler::journal::record_type;

std::span<const std::byte> bytes_of(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

std::string text_of(std::span<const std::byte> payload) {
    return {std::bit_cast<const char*>(payload.data()), payload.size()};
}

constexpr std::string_view kSnapshot =
    R"({"channel":"level3","type":"snapshot","data":[{"symbol":"BTC/USD"}]})";
constexpr std::string_view kUpdate =
    R"({"channel":"level3","type":"update","data":[{"event":"add"}],"checksum":1671312190})";

}  // namespace

// Fixtures and tests at namespace scope: cppcheck cannot parse TEST macros
// that follow another definition inside an anonymous namespace (see
// journal_test.cpp).

class CaptureSessionDir : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("capture_session_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    std::filesystem::path dir_;
};

TEST_F(CaptureSessionDir, CreatesTheDirectoryAndOpensAFilePerIncarnation) {
    capture_session session({.directory = dir_, .exchange = "kraken"});
    EXPECT_EQ(session.incarnation(), 0U);

    const auto first = session.begin_incarnation("connected");
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_EQ(session.incarnation(), 1U);
    EXPECT_TRUE(std::filesystem::exists(*first));

    const auto second = session.begin_incarnation("reconnected");
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_EQ(session.incarnation(), 2U);
    EXPECT_NE(*first, *second);
    // The previous incarnation's file stays on disk, complete: one file per
    // (exchange, connection-incarnation), never reopened or appended to.
    EXPECT_TRUE(std::filesystem::exists(*first));
}

TEST_F(CaptureSessionDir, WritesTheIncarnationMarkerAsTheFirstRecordOfEveryFile) {
    capture_session session({.directory = dir_, .exchange = "kraken"});

    const auto first = session.begin_incarnation("connected");
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_TRUE(session.on_wire_message(bytes_of(kSnapshot)));
    EXPECT_TRUE(session.on_wire_message(bytes_of(kUpdate)));

    const auto second = session.begin_incarnation("staleness watchdog");
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_TRUE(session.on_wire_message(bytes_of(kSnapshot)));
    session.close();

    auto reader = journal_reader::open(*first);
    ASSERT_TRUE(reader.has_value()) << reader.error();
    EXPECT_EQ(reader->header().incarnation, 1U);
    EXPECT_EQ(reader->header().exchange, "kraken");

    auto marker = reader->next();
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->type, record_type::connection_incarnation);
    EXPECT_EQ(text_of(marker->payload), "connected");
    EXPECT_EQ(marker->capture_sequence, 1U);

    auto snapshot = reader->next();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->type, record_type::wire_message);
    EXPECT_EQ(text_of(snapshot->payload), kSnapshot);
    EXPECT_EQ(snapshot->capture_sequence, 2U);
    ASSERT_TRUE(reader->next().has_value());
    EXPECT_FALSE(reader->next().has_value());
    EXPECT_FALSE(reader->stopped_early()) << reader->stop_reason();

    auto second_reader = journal_reader::open(*second);
    ASSERT_TRUE(second_reader.has_value()) << second_reader.error();
    EXPECT_EQ(second_reader->header().incarnation, 2U);
    auto second_marker = second_reader->next();
    ASSERT_TRUE(second_marker.has_value());
    EXPECT_EQ(second_marker->type, record_type::connection_incarnation);
    EXPECT_EQ(text_of(second_marker->payload), "staleness watchdog");
    // Capture sequence numbers are per-incarnation, so the new file restarts
    // at 1 rather than continuing the previous file's numbering.
    EXPECT_EQ(second_marker->capture_sequence, 1U);
}

TEST_F(CaptureSessionDir, CountsRecordsAcrossIncarnations) {
    capture_session session({.directory = dir_, .exchange = "kraken"});
    ASSERT_TRUE(session.begin_incarnation("connected").has_value());
    session.on_wire_message(bytes_of(kUpdate));
    EXPECT_EQ(session.records_written(), 2U);  // marker + one message

    ASSERT_TRUE(session.begin_incarnation("reconnected").has_value());
    session.on_wire_message(bytes_of(kUpdate));
    EXPECT_EQ(session.records_written(), 2U);
    EXPECT_EQ(session.total_records_written(), 4U);

    session.close();
    EXPECT_EQ(session.total_records_written(), 4U);
}

TEST_F(CaptureSessionDir, DropsMessagesArrivingBeforeAnIncarnationIsOpen) {
    capture_session session({.directory = dir_, .exchange = "kraken"});
    // Reported rather than silently swallowed: the caller logs it.
    EXPECT_FALSE(session.on_wire_message(bytes_of(kUpdate)));
    EXPECT_EQ(session.total_records_written(), 0U);
}

TEST_F(CaptureSessionDir, ReportsAnUnusableDirectoryInsteadOfThrowing) {
    // A path whose parent is a regular file cannot become a directory.
    std::filesystem::create_directories(dir_);
    const std::filesystem::path blocker = dir_ / "not-a-dir";
    { std::ofstream file(blocker); }

    capture_session session({.directory = blocker / "journal", .exchange = "kraken"});
    const auto started = session.begin_incarnation("connected");
    EXPECT_FALSE(started.has_value());
    EXPECT_FALSE(started.error().empty());
}

TEST(CaptureSessionNaming, MakesExchangeAndIncarnationObviousFromTheFileName) {
    // 2026-09-16T21:30:00Z.
    constexpr std::uint64_t kRealtimeNs = 1'789'594'200ULL * 1'000'000'000ULL;
    EXPECT_EQ(feed_handler::journal_file_name("kraken", 3, kRealtimeNs),
              "kraken-000003-20260916T213000Z.journal");
    // Zero-padded so a directory listing sorts in connection order.
    EXPECT_EQ(feed_handler::journal_file_name("kraken", 12, kRealtimeNs),
              "kraken-000012-20260916T213000Z.journal");
}
