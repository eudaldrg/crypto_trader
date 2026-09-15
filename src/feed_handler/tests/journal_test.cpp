// Round-trip and crash-tail behavior of the v1 capture journal
// (decisions/0004-feed-handler-architecture.md, "Journal format v1").
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "feed_handler/journal_format.h"
#include "feed_handler/journal_reader.h"
#include "feed_handler/journal_writer.h"
#include "feed_handler/message_sink.h"

namespace {

using feed_handler::capture_frame;
using feed_handler::capture_stamper;
using feed_handler::journal_reader;
using feed_handler::journal_writer;
using feed_handler::journal::record_type;

std::span<const std::byte> bytes_of(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

std::string text_of(std::span<const std::byte> payload) {
    return {std::bit_cast<const char*>(payload.data()), payload.size()};
}

// A realistic-ish sample of what actually lands on the wire: Kraken level3
// JSON, journaled verbatim with no re-encoding (decisions/0004).
constexpr std::string_view kSnapshot =
    R"({"channel":"level3","type":"snapshot","data":[{"symbol":"BTC/USD"}]})";
constexpr std::string_view kUpdate =
    R"({"channel":"level3","type":"update","data":[{"event":"add","order_id":"OABC"}],)"
    R"("checksum":1671312190})";
constexpr std::string_view kHeartbeat = R"({"channel":"heartbeat"})";

}  // namespace

// The fixture and the tests live at namespace scope rather than inside the
// anonymous namespace above: cppcheck cannot parse GoogleTest's TEST macros
// when they follow another definition inside an anonymous namespace, and the
// pre-commit cppcheck hook treats that as a hard error.

/// Gives each test its own file under the build tree's temp dir and removes it
/// afterwards, so tests stay independent and leave nothing behind.
class JournalFile : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ =
            std::filesystem::temp_directory_path() /
            ("journal_test_" +
             std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + ".bin");
        std::filesystem::remove(path_);
    }

    void TearDown() override {
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
};

TEST_F(JournalFile, RoundTripsHeaderRecordsAndIncarnationMarker) {
    capture_stamper stamper;
    {
        journal_writer writer(path_, {.exchange = "kraken", .incarnation = 7});
        writer.write_incarnation_marker(stamper.stamp(bytes_of("connect")));
        writer.on_frame(stamper.stamp(bytes_of(kSnapshot)));
        writer.on_frame(stamper.stamp(bytes_of(kUpdate)));
        writer.on_frame(stamper.stamp(bytes_of(kHeartbeat)));
        writer.flush();
        ASSERT_TRUE(writer.good()) << writer.error();
        EXPECT_EQ(writer.records_written(), 4U);
    }

    auto reader = journal_reader::open(path_);
    ASSERT_TRUE(reader.has_value()) << reader.error();
    EXPECT_EQ(reader->header().format_version, feed_handler::journal::kFormatVersion);
    EXPECT_EQ(reader->header().exchange, "kraken");
    EXPECT_EQ(reader->header().incarnation, 7U);
    // The wall-clock anchor must actually be populated; monotonic alone cannot
    // be correlated across a restart.
    EXPECT_GT(reader->header().realtime_ns, 0U);
    EXPECT_GT(reader->header().monotonic_ns, 0U);

    auto marker = reader->next();
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->type, record_type::connection_incarnation);
    EXPECT_EQ(text_of(marker->payload), "connect");
    EXPECT_EQ(marker->capture_sequence, 1U);

    const std::array<std::string_view, 3> expected = {kSnapshot, kUpdate, kHeartbeat};
    std::uint64_t previous_sequence = marker->capture_sequence;
    for (std::string_view want : expected) {
        auto record = reader->next();
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->type, record_type::wire_message);
        EXPECT_EQ(text_of(record->payload), want);
        EXPECT_GT(record->capture_sequence, previous_sequence);
        EXPECT_GE(record->monotonic_ns, reader->header().monotonic_ns);
        previous_sequence = record->capture_sequence;
    }

    EXPECT_FALSE(reader->next().has_value());
    EXPECT_FALSE(reader->stopped_early()) << reader->stop_reason();
    EXPECT_EQ(reader->records_read(), 4U);
}

TEST_F(JournalFile, EmptyJournalIsStillAValidFile) {
    { journal_writer writer(path_, {.exchange = "kraken"}); }

    auto reader = journal_reader::open(path_);
    ASSERT_TRUE(reader.has_value()) << reader.error();
    EXPECT_FALSE(reader->next().has_value());
    EXPECT_FALSE(reader->stopped_early());
}

TEST_F(JournalFile, PreservesEmptyAndBinaryPayloads) {
    const std::array<std::byte, 4> binary = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x0A},
                                             std::byte{0x00}};
    capture_stamper stamper;
    {
        journal_writer writer(path_, {.exchange = "kraken"});
        writer.on_frame(stamper.stamp({}));
        writer.on_frame(stamper.stamp(binary));
        writer.flush();
        ASSERT_TRUE(writer.good()) << writer.error();
    }

    auto reader = journal_reader::open(path_);
    ASSERT_TRUE(reader.has_value()) << reader.error();

    auto empty = reader->next();
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->payload.empty());

    auto blob = reader->next();
    ASSERT_TRUE(blob.has_value());
    ASSERT_EQ(blob->payload.size(), binary.size());
    EXPECT_TRUE(std::equal(binary.begin(), binary.end(), blob->payload.begin()));

    EXPECT_FALSE(reader->next().has_value());
    EXPECT_FALSE(reader->stopped_early());
}

TEST_F(JournalFile, StopsCleanlyOnTruncatedTail) {
    capture_stamper stamper;
    {
        journal_writer writer(path_, {.exchange = "kraken"});
        writer.on_frame(stamper.stamp(bytes_of(kSnapshot)));
        writer.on_frame(stamper.stamp(bytes_of(kUpdate)));
        writer.flush();
    }

    // Simulate a crash mid-write of the last record by lopping bytes off the
    // end of the file.
    const auto full_size = std::filesystem::file_size(path_);
    std::filesystem::resize_file(path_, full_size - 10);

    auto reader = journal_reader::open(path_);
    ASSERT_TRUE(reader.has_value()) << reader.error();

    auto first = reader->next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(text_of(first->payload), kSnapshot);

    EXPECT_FALSE(reader->next().has_value());
    EXPECT_TRUE(reader->stopped_early());
    EXPECT_FALSE(reader->stop_reason().empty());
    EXPECT_EQ(reader->records_read(), 1U);
    // Truncation must not be reported as a hard error, only as a short read.
    EXPECT_FALSE(reader->next().has_value());
}

TEST_F(JournalFile, StopsCleanlyOnCorruptedPayload) {
    capture_stamper stamper;
    {
        journal_writer writer(path_, {.exchange = "kraken"});
        writer.on_frame(stamper.stamp(bytes_of(kSnapshot)));
        writer.on_frame(stamper.stamp(bytes_of(kUpdate)));
        writer.flush();
    }

    // Flip a byte inside the second record's payload, leaving every length
    // field intact: only the per-record CRC can catch this.
    const auto second_record_payload_start =
        feed_handler::journal::kFileHeaderSize + feed_handler::journal::kRecordHeaderSize +
        kSnapshot.size() + feed_handler::journal::kRecordTrailerSize +
        feed_handler::journal::kRecordHeaderSize;
    {
        std::fstream file(path_, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(static_cast<std::streamoff>(second_record_payload_start + 3));
        const char flipped = 'X';
        file.write(&flipped, 1);
    }

    auto reader = journal_reader::open(path_);
    ASSERT_TRUE(reader.has_value()) << reader.error();
    ASSERT_TRUE(reader->next().has_value());
    EXPECT_FALSE(reader->next().has_value());
    EXPECT_TRUE(reader->stopped_early());
    EXPECT_EQ(reader->stop_reason(), "record checksum mismatch");
}

TEST_F(JournalFile, RejectsNonJournalFile) {
    {
        std::ofstream file(path_, std::ios::binary);
        const std::string junk(128, 'z');
        file.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    auto reader = journal_reader::open(path_);
    EXPECT_FALSE(reader.has_value());
}

TEST_F(JournalFile, RejectsCorruptedFileHeader) {
    { journal_writer writer(path_, {.exchange = "kraken"}); }
    {
        std::fstream file(path_, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(20);
        const char flipped = '\x7F';
        file.write(&flipped, 1);
    }
    auto reader = journal_reader::open(path_);
    EXPECT_FALSE(reader.has_value());
}

TEST_F(JournalFile, ThrowsWhenFileCannotBeCreated) {
    const std::filesystem::path bad = path_ / "no" / "such" / "dir" / "j.bin";
    EXPECT_THROW(journal_writer(bad, {.exchange = "kraken"}), std::runtime_error);
}

TEST_F(JournalFile, RefusesOutOfOrderCaptureSequence) {
    journal_writer writer(path_, {.exchange = "kraken"});
    const capture_frame first{.payload = bytes_of(kSnapshot), .capture_sequence = 5};
    const capture_frame stale{.payload = bytes_of(kUpdate), .capture_sequence = 5};

    writer.on_frame(first);
    EXPECT_TRUE(writer.good());
    writer.on_frame(stale);
    EXPECT_FALSE(writer.good());
    EXPECT_EQ(writer.records_written(), 1U);
}

TEST(CaptureStamper, AssignsStrictlyIncreasingSequenceNumbers) {
    capture_stamper stamper;
    EXPECT_EQ(stamper.last_sequence(), 0U);

    std::uint64_t previous = 0;
    for (int iteration = 0; iteration < 1000; ++iteration) {
        const capture_frame frame = stamper.stamp(bytes_of(kHeartbeat));
        EXPECT_GT(frame.capture_sequence, previous);
        previous = frame.capture_sequence;
    }
    EXPECT_EQ(stamper.last_sequence(), 1000U);
}

TEST(CaptureStamper, StampsAMonotonicTimestampAndDoesNotCopyThePayload) {
    capture_stamper stamper;
    const std::string payload(kUpdate);
    const capture_frame frame = stamper.stamp(bytes_of(payload));

    EXPECT_GT(frame.monotonic_ns, 0U);
    EXPECT_GE(feed_handler::monotonic_now_ns(), frame.monotonic_ns);
    // The frame must be a view, not a copy -- the ownership contract the whole
    // MessageSink seam depends on.
    EXPECT_EQ(static_cast<const void*>(frame.payload.data()),
              static_cast<const void*>(payload.data()));
}

TEST(JournalFormat, LittleEndianRoundTrip) {
    std::array<std::byte, 8> raw{};
    feed_handler::journal::store_le<std::uint64_t>(raw, 0x0123456789ABCDEFULL);
    EXPECT_EQ(raw[0], std::byte{0xEF});
    EXPECT_EQ(raw[7], std::byte{0x01});
    EXPECT_EQ(feed_handler::journal::load_le<std::uint64_t>(raw), 0x0123456789ABCDEFULL);
}
