// Incarnation/journal-rotation bookkeeping and sink fan-out: the parts of the
// reconnect path that can be tested without a socket. decisions/0004 requires
// one file per (exchange, connection-incarnation), an explicit marker record at
// the start of each, and per-incarnation capture sequence numbers; the sink
// fan-out is what the order book will attach to.
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
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/recording_sink.h"

namespace {

using feed_handler::CaptureSession;
using feed_handler::FrameSource;
using feed_handler::JournalReader;
using feed_handler::journal::RecordType;
using feed_handler::testing::RecordingSink;

/// These tests are not about any particular exchange; they only need a value
/// that is not the default, so that "the session passed the source through"
/// cannot be confused with "nobody set it".
constexpr FrameSource kSource = FrameSource::kRakenJson;

std::span<const std::byte> BytesOf(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

std::string TextOf(std::span<const std::byte> payload) {
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
    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    EXPECT_EQ(session.Incarnation(), 0U);

    const auto first = session.BeginIncarnation("connected", kSource);
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_EQ(session.Incarnation(), 1U);
    EXPECT_TRUE(std::filesystem::exists(*first));

    const auto second = session.BeginIncarnation("reconnected", kSource);
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_EQ(session.Incarnation(), 2U);
    EXPECT_NE(*first, *second);
    // The previous incarnation's file stays on disk, complete: one file per
    // (exchange, connection-incarnation), never reopened or appended to.
    EXPECT_TRUE(std::filesystem::exists(*first));
}

TEST_F(CaptureSessionDir, FilePrefixNamesTheFileButNotTheHeaderExchange) {
    // Two connections to one exchange, same directory, both on incarnation 1:
    // only distinct prefixes keep their files apart.
    CaptureSession first({.directory = dir_, .exchange = "kraken"}, "kraken-btc");
    CaptureSession second({.directory = dir_, .exchange = "kraken"}, "kraken-eth");

    const auto first_path = first.BeginIncarnation("connected", kSource);
    const auto second_path = second.BeginIncarnation("connected", kSource);
    ASSERT_TRUE(first_path.has_value()) << first_path.error();
    ASSERT_TRUE(second_path.has_value()) << second_path.error();
    EXPECT_NE(*first_path, *second_path);
    EXPECT_TRUE(first_path->filename().string().starts_with("kraken-btc-000001-"));
    EXPECT_TRUE(second_path->filename().string().starts_with("kraken-eth-000001-"));
    first.Close();
    second.Close();

    auto reader = JournalReader::Open(*first_path);
    ASSERT_TRUE(reader.has_value()) << reader.error();
    EXPECT_EQ(reader->Header().exchange, "kraken");
}

TEST_F(CaptureSessionDir, WritesTheIncarnationMarkerAsTheFirstRecordOfEveryFile) {
    CaptureSession session({.directory = dir_, .exchange = "kraken"});

    const auto first = session.BeginIncarnation("connected", kSource);
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_TRUE(session.OnWireMessage(BytesOf(kSnapshot), kSource));
    EXPECT_TRUE(session.OnWireMessage(BytesOf(kUpdate), kSource));

    const auto second = session.BeginIncarnation("staleness watchdog", kSource);
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_TRUE(session.OnWireMessage(BytesOf(kSnapshot), kSource));
    session.Close();

    auto reader = JournalReader::Open(*first);
    ASSERT_TRUE(reader.has_value()) << reader.error();
    EXPECT_EQ(reader->Header().incarnation, 1U);
    EXPECT_EQ(reader->Header().exchange, "kraken");

    auto marker = reader->Next();
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->type, RecordType::kConnectionIncarnation);
    EXPECT_EQ(TextOf(marker->payload), "connected");
    EXPECT_EQ(marker->capture_sequence, 1U);

    auto snapshot = reader->Next();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->type, RecordType::kWireMessage);
    EXPECT_EQ(TextOf(snapshot->payload), kSnapshot);
    EXPECT_EQ(snapshot->capture_sequence, 2U);
    ASSERT_TRUE(reader->Next().has_value());
    EXPECT_FALSE(reader->Next().has_value());
    EXPECT_FALSE(reader->StoppedEarly()) << reader->StopReason();

    auto second_reader = JournalReader::Open(*second);
    ASSERT_TRUE(second_reader.has_value()) << second_reader.error();
    EXPECT_EQ(second_reader->Header().incarnation, 2U);
    auto second_marker = second_reader->Next();
    ASSERT_TRUE(second_marker.has_value());
    EXPECT_EQ(second_marker->type, RecordType::kConnectionIncarnation);
    EXPECT_EQ(TextOf(second_marker->payload), "staleness watchdog");
    // Capture sequence numbers are per-incarnation, so the new file restarts
    // at 1 rather than continuing the previous file's numbering.
    EXPECT_EQ(second_marker->capture_sequence, 1U);
}

TEST_F(CaptureSessionDir, CountsRecordsAcrossIncarnations) {
    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    ASSERT_TRUE(session.BeginIncarnation("connected", kSource).has_value());
    session.OnWireMessage(BytesOf(kUpdate), kSource);
    EXPECT_EQ(session.RecordsWritten(), 2U);  // marker + one message

    ASSERT_TRUE(session.BeginIncarnation("reconnected", kSource).has_value());
    session.OnWireMessage(BytesOf(kUpdate), kSource);
    EXPECT_EQ(session.RecordsWritten(), 2U);
    EXPECT_EQ(session.TotalRecordsWritten(), 4U);

    session.Close();
    EXPECT_EQ(session.TotalRecordsWritten(), 4U);
}

TEST_F(CaptureSessionDir, DropsMessagesArrivingBeforeAnIncarnationIsOpen) {
    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    // Reported rather than silently swallowed: the caller logs it.
    EXPECT_FALSE(session.OnWireMessage(BytesOf(kUpdate), kSource));
    EXPECT_EQ(session.TotalRecordsWritten(), 0U);
}

TEST_F(CaptureSessionDir, ReportsAnUnusableDirectoryInsteadOfThrowing) {
    // A path whose parent is a regular file cannot become a directory.
    std::filesystem::create_directories(dir_);
    const std::filesystem::path blocker = dir_ / "not-a-dir";
    { std::ofstream file(blocker); }

    CaptureSession session({.directory = blocker / "journal", .exchange = "kraken"});
    const auto started = session.BeginIncarnation("connected", kSource);
    EXPECT_FALSE(started.has_value());
    EXPECT_FALSE(started.error().empty());
}

TEST_F(CaptureSessionDir, DeliversEveryFrameToEveryRegisteredSink) {
    // The seam the order book plugs into: more than one sink, all of them fed
    // from the same capture call, none of them the journal writer.
    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    RecordingSink first;
    RecordingSink second;
    session.AddSink(first);
    session.AddSink(second);

    ASSERT_TRUE(session.BeginIncarnation("connected", kSource).has_value());
    EXPECT_TRUE(session.OnWireMessage(BytesOf(kSnapshot), kSource));
    EXPECT_TRUE(session.OnWireMessage(BytesOf(kUpdate), kSource));

    for (const RecordingSink* sink : {&first, &second}) {
        const auto frames = sink->Frames();
        ASSERT_EQ(frames.size(), 2U);
        EXPECT_EQ(frames[0].payload, kSnapshot);
        EXPECT_EQ(frames[1].payload, kUpdate);
        EXPECT_EQ(frames[0].source, kSource);
        EXPECT_EQ(frames[1].source, kSource);
        // The same capture sequence the journal recorded: the marker took 1,
        // so the two wire messages are 2 and 3. A sink and the journal
        // therefore agree on message identity without any extra bookkeeping.
        EXPECT_EQ(frames[0].capture_sequence, 2U);
        EXPECT_EQ(frames[1].capture_sequence, 3U);
        EXPECT_NE(frames[0].monotonic_ns, 0U);
        // The incarnation marker is a journal record, not a frame: a sink is
        // told about the incarnation, it is not handed the marker's payload.
        EXPECT_EQ(sink->Incarnations().size(), 1U);
    }
}

TEST_F(CaptureSessionDir, JournalsEveryFrameItFansOutToSinks) {
    // The other half of the previous test: fan-out must not cost the journal a
    // record, since the journal is what makes capture durable.
    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    RecordingSink sink;
    session.AddSink(sink);

    const auto path = session.BeginIncarnation("connected", kSource);
    ASSERT_TRUE(path.has_value()) << path.error();
    EXPECT_TRUE(session.OnWireMessage(BytesOf(kSnapshot), kSource));
    session.Close();

    auto reader = JournalReader::Open(*path);
    ASSERT_TRUE(reader.has_value()) << reader.error();
    ASSERT_TRUE(reader->Next().has_value());  // the marker
    auto record = reader->Next();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(TextOf(record->payload), kSnapshot);
    EXPECT_EQ(sink.Frames().size(), 1U);
}

TEST_F(CaptureSessionDir, TellsEverySinkAboutEveryIncarnation) {
    // A reconnect is the one event a stateful sink cannot be correct without:
    // it is when an order book has to throw away the book it built from the
    // previous connection and wait for the fresh snapshot.
    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    RecordingSink first;
    RecordingSink second;
    session.AddSink(first);
    session.AddSink(second);

    ASSERT_TRUE(session.BeginIncarnation("connected", kSource).has_value());
    ASSERT_TRUE(session.BeginIncarnation("staleness watchdog", kSource).has_value());

    for (const RecordingSink* sink : {&first, &second}) {
        const auto seen = sink->Incarnations();
        ASSERT_EQ(seen.size(), 2U);
        EXPECT_EQ(seen[0].incarnation, 1U);
        EXPECT_EQ(seen[0].reason, "connected");
        EXPECT_EQ(seen[1].incarnation, 2U);
        // Verbatim, and the same text the marker record carries, so a journal
        // and a live sink describe the same reconnect the same way.
        EXPECT_EQ(seen[1].reason, "staleness watchdog");
    }
}

TEST_F(CaptureSessionDir, DoesNotAnnounceAnIncarnationThatFailedToStart) {
    // The caller treats a failed begin_incarnation as fatal to capture, so
    // telling a sink to reset for a connection that never starts would leave it
    // resetting on a lie.
    std::filesystem::create_directories(dir_);
    const std::filesystem::path blocker = dir_ / "not-a-dir";
    { std::ofstream file(blocker); }

    CaptureSession session({.directory = blocker / "journal", .exchange = "kraken"});
    RecordingSink sink;
    session.AddSink(sink);

    EXPECT_FALSE(session.BeginIncarnation("connected", kSource).has_value());
    EXPECT_TRUE(sink.Incarnations().empty());
}

TEST(CaptureSessionNaming, MakesExchangeAndIncarnationObviousFromTheFileName) {
    // 2026-09-16T21:30:00Z.
    constexpr std::uint64_t kRealtimeNs = 1'789'594'200ULL * 1'000'000'000ULL;
    EXPECT_EQ(feed_handler::JournalFileName("kraken", 3, kRealtimeNs),
              "kraken-000003-20260916T213000Z.journal");
    // Zero-padded so a directory listing sorts in connection order.
    EXPECT_EQ(feed_handler::JournalFileName("kraken", 12, kRealtimeNs),
              "kraken-000012-20260916T213000Z.journal");
}
