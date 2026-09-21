// The journal on its own thread (journal_thread.h) and the kThreaded mode of
// CaptureSession built on it. The properties that matter: the file is
// byte-for-byte what inline mode writes, Close() is a barrier, and a full ring or
// a failed write is fatal and reported once. None of it may rest on a sleep: the
// tests order things with the barrier, the destructor's join, or a journal thread
// that has not been started yet.
#include "feed_handler/journal_thread.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "feed_handler/capture_session.h"
#include "feed_handler/journal_reader.h"
#include "feed_handler/journal_writer.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/recording_sink.h"

namespace {

using feed_handler::CaptureFrame;
using feed_handler::CaptureSession;
using feed_handler::CaptureStamper;
using feed_handler::FrameSource;
using feed_handler::JournalMode;
using feed_handler::JournalReader;
using feed_handler::JournalThread;
using feed_handler::JournalWriter;
using feed_handler::journal::RecordType;
using feed_handler::testing::RecordingSink;

constexpr FrameSource kSource = FrameSource::kKrakenJson;
constexpr std::string_view kDevFull = "/dev/full";
constexpr std::size_t kSmallRing = 4;
constexpr std::size_t kManyFrames = 300;
constexpr std::size_t kMoreThanAnyRingHolds = 1'000'000;
constexpr std::size_t kBigPayloadBytes = 2U << 20U;
constexpr std::size_t kOverflowLoopBound = 1'000;

constexpr std::string_view kSnapshot =
    R"({"channel":"level3","type":"snapshot","data":[{"symbol":"BTC/USD"}]})";
constexpr std::string_view kUpdate =
    R"({"channel":"level3","type":"update","data":[{"event":"add"}],"checksum":1671312190})";

std::span<const std::byte> BytesOf(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

std::string TextOf(std::span<const std::byte> payload) {
    return payload.empty()
               ? std::string{}
               : std::string(std::bit_cast<const char*>(payload.data()), payload.size());
}

/// A payload that differs from frame to frame and, every seventh, is empty, so a
/// writer that mishandled either would show.
std::string PayloadFor(std::size_t index) {
    if (index % 7 == 6) {
        return {};
    }
    return std::string(kUpdate) + std::to_string(index) + std::string(index % 50, 'x');
}

std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

void Zero(std::vector<std::byte>& bytes, std::size_t offset, std::size_t count) {
    for (std::size_t index = offset; index < offset + count && index < bytes.size(); ++index) {
        bytes[index] = std::byte{0};
    }
}

/// The file's bytes with the fields that come from the clock zeroed: the header's
/// realtime/monotonic anchors and CRC and, when `records_too`, every record's
/// monotonic timestamp and CRC. What is left is exactly what two runs of the same
/// frames must agree on.
std::vector<std::byte> WithoutClockFields(const std::filesystem::path& path, bool records_too) {
    namespace journal = feed_handler::journal;
    std::vector<std::byte> bytes = ReadBytes(path);
    Zero(bytes, 16, 16);
    Zero(bytes, 60, 4);
    if (!records_too) {
        return bytes;
    }
    std::size_t offset = journal::kFileHeaderSize;
    while (offset + journal::kRecordHeaderSize + journal::kRecordTrailerSize <= bytes.size()) {
        const auto payload_bytes = journal::LoadLe<std::uint32_t>(
            std::span<const std::byte>(bytes).subspan(offset + 4, sizeof(std::uint32_t)));
        Zero(bytes, offset + 16, sizeof(std::uint64_t));
        const std::size_t crc_at = offset + journal::kRecordHeaderSize + payload_bytes;
        Zero(bytes, crc_at, journal::kRecordTrailerSize);
        offset = crc_at + journal::kRecordTrailerSize;
    }
    return bytes;
}

struct ReadRecord {
    RecordType type;
    std::uint64_t capture_sequence;
    std::string payload;
};

/// Every record of `path`, failing the test if the file does not read to a clean end.
std::vector<ReadRecord> ReadAllRecords(const std::filesystem::path& path) {
    std::vector<ReadRecord> records;
    auto reader = JournalReader::Open(path);
    EXPECT_TRUE(reader.has_value());
    if (!reader.has_value()) {
        return records;
    }
    while (auto record = reader->Next()) {
        records.push_back({record->type, record->capture_sequence, TextOf(record->payload)});
    }
    EXPECT_FALSE(reader->StoppedEarly()) << reader->StopReason();
    return records;
}

/// What a fatal callback saw. Thread safe: it is called from another thread.
class FatalRecorder {
  public:
    JournalThread::FatalCallback Callback() {
        return [this](std::string_view reason) { Record(reason); };
    }

    void Record(std::string_view reason) {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++calls_;
        reason_ = std::string(reason);
        thread_ = std::this_thread::get_id();
    }

    int Calls() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    std::string Reason() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return reason_;
    }

    std::thread::id Thread() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return thread_;
    }

  private:
    mutable std::mutex mutex_;
    int calls_ = 0;
    std::string reason_;
    std::thread::id thread_;
};

}  // namespace

// Fixture and tests at namespace scope (cppcheck, see journal_test.cpp).

class JournalThreadDir : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("journal_thread_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    static CaptureSession::Config Threaded(const std::filesystem::path& dir) {
        return {.directory = dir, .exchange = "kraken", .journal_mode = JournalMode::kThreaded};
    }

    std::filesystem::path dir_;
};

TEST_F(JournalThreadDir, WriterFedThroughTheThreadWritesTheSameBytesAsOneFedDirectly) {
    // Same stamped frames into a plain JournalWriter and into one behind the
    // thread: the only bytes allowed to differ are the header's clock anchors.
    CaptureStamper stamper;
    const CaptureFrame marker = stamper.Stamp(BytesOf("connected"), kSource);
    std::vector<std::string> payloads;
    std::vector<CaptureFrame> frames;
    for (std::size_t index = 0; index < kManyFrames; ++index) {
        payloads.push_back(PayloadFor(index));
    }
    for (const std::string& payload : payloads) {
        frames.push_back(stamper.Stamp(BytesOf(payload), kSource));
    }

    const std::filesystem::path direct_path = dir_ / "direct.journal";
    {
        JournalWriter direct(direct_path, {.exchange = "kraken", .connect_id = 1});
        direct.WriteConnectMarker(marker);
        for (const CaptureFrame& frame : frames) {
            direct.OnFrame(frame);
        }
        direct.Flush();
    }

    const std::filesystem::path threaded_path = dir_ / "threaded.journal";
    {
        JournalThread journal({});
        journal.Connect(
            std::make_unique<JournalWriter>(
                threaded_path, JournalWriter::Config{.exchange = "kraken", .connect_id = 1}),
            marker);
        for (const CaptureFrame& frame : frames) {
            ASSERT_TRUE(journal.Push(frame));
        }
        EXPECT_EQ(journal.Disconnect(), kManyFrames + 1);
    }

    const std::vector<std::byte> direct_bytes = WithoutClockFields(direct_path, false);
    const std::vector<std::byte> threaded_bytes = WithoutClockFields(threaded_path, false);
    ASSERT_FALSE(direct_bytes.empty());
    EXPECT_EQ(direct_bytes.size(), threaded_bytes.size());
    EXPECT_TRUE(direct_bytes == threaded_bytes);
}

TEST_F(JournalThreadDir, SessionFilesMatchInlineModeAcrossAReconnect) {
    // The whole session path, reconnect included, in both modes. The clock is the
    // one thing two runs cannot share, so its fields are masked in every record.
    const std::filesystem::path inline_dir = dir_ / "inline";
    const std::filesystem::path threaded_dir = dir_ / "threaded";
    std::vector<std::filesystem::path> inline_files;
    std::vector<std::filesystem::path> threaded_files;

    const auto run = [](CaptureSession& session, std::vector<std::filesystem::path>& files) {
        for (const char* reason : {"connected", "staleness watchdog"}) {
            const auto path = session.BeginConnect(reason, kSource);
            ASSERT_TRUE(path.has_value()) << path.error();
            files.push_back(*path);
            for (std::size_t index = 0; index < kManyFrames; ++index) {
                ASSERT_TRUE(session.OnWireMessage(BytesOf(PayloadFor(index)), kSource));
            }
        }
        session.Close();
    };

    CaptureSession inline_session({.directory = inline_dir, .exchange = "kraken"});
    run(inline_session, inline_files);
    CaptureSession threaded_session(Threaded(threaded_dir));
    run(threaded_session, threaded_files);

    ASSERT_EQ(inline_files.size(), 2U);
    ASSERT_EQ(threaded_files.size(), 2U);
    for (std::size_t file = 0; file < 2; ++file) {
        const std::vector<std::byte> want = WithoutClockFields(inline_files[file], true);
        const std::vector<std::byte> got = WithoutClockFields(threaded_files[file], true);
        ASSERT_FALSE(want.empty());
        EXPECT_EQ(want.size(), got.size()) << "file " << file;
        EXPECT_TRUE(want == got) << "file " << file;
    }
    EXPECT_EQ(threaded_session.TotalRecordsWritten(), inline_session.TotalRecordsWritten());
    EXPECT_EQ(threaded_session.TotalRecordsWritten(), 2 * (kManyFrames + 1));
}

TEST_F(JournalThreadDir, CloseIsABarrierTheFileIsCompleteWhenItReturns) {
    // Read the file the instant Close() returns, and again from inside the
    // OnDisconnect it delivers: both must see every record, closed cleanly.
    class CompleteChecker final : public feed_handler::MessageSink {
      public:
        void OnFrame(const CaptureFrame& /*frame*/) override {}
        void OnDisconnect(std::uint64_t /*connect_id*/) override {
            records_ = ReadAllRecords(path_).size();
        }
        void Watch(const std::filesystem::path& path) {
            path_ = path;
        }
        std::size_t Records() const {
            return records_;
        }

      private:
        std::filesystem::path path_;
        std::size_t records_ = 0;
    };

    CaptureSession session(Threaded(dir_));
    CompleteChecker checker;
    session.AddSink(checker);
    const auto path = session.BeginConnect("connected", kSource);
    ASSERT_TRUE(path.has_value()) << path.error();
    checker.Watch(*path);
    for (std::size_t index = 0; index < kManyFrames; ++index) {
        ASSERT_TRUE(session.OnWireMessage(BytesOf(PayloadFor(index)), kSource));
    }
    session.Close();

    EXPECT_EQ(checker.Records(), kManyFrames + 1);
    const auto records = ReadAllRecords(*path);
    ASSERT_EQ(records.size(), kManyFrames + 1);
    EXPECT_EQ(records.front().type, RecordType::kConnect);
    EXPECT_EQ(records.front().payload, "connected");
    for (std::size_t index = 0; index < kManyFrames; ++index) {
        EXPECT_EQ(records[index + 1].type, RecordType::kWireMessage);
        EXPECT_EQ(records[index + 1].capture_sequence, index + 2);
        EXPECT_EQ(records[index + 1].payload, PayloadFor(index));
    }
    // The accessors are exact once the barrier has passed.
    EXPECT_EQ(session.RecordsWritten(), 0U);
    EXPECT_EQ(session.TotalRecordsWritten(), kManyFrames + 1);
    EXPECT_TRUE(session.Error().empty());
    EXPECT_TRUE(session.CurrentPath().empty());
}

TEST_F(JournalThreadDir, CloseTwiceIsHarmless) {
    CaptureSession session(Threaded(dir_));
    RecordingSink sink;
    session.AddSink(sink);
    ASSERT_TRUE(session.BeginConnect("connected", kSource).has_value());
    session.Close();
    session.Close();
    EXPECT_EQ(sink.Disconnects(), (std::vector<std::uint64_t>{1}));
}

TEST_F(JournalThreadDir, DrainsEverythingQueuedWhenTheSessionIsDestroyed) {
    // No Close(): destroying the session joins the journal thread, which first
    // drains the ring and flushes the open file.
    std::filesystem::path path;
    {
        CaptureSession session(Threaded(dir_));
        const auto begun = session.BeginConnect("connected", kSource);
        ASSERT_TRUE(begun.has_value()) << begun.error();
        path = *begun;
        for (std::size_t index = 0; index < kManyFrames; ++index) {
            ASSERT_TRUE(session.OnWireMessage(BytesOf(PayloadFor(index)), kSource));
        }
    }
    EXPECT_EQ(ReadAllRecords(path).size(), kManyFrames + 1);
}

TEST_F(JournalThreadDir, ASessionThatNeverConnectedShutsDownCleanly) {
    CaptureSession session(Threaded(dir_));
    EXPECT_FALSE(session.OnWireMessage(BytesOf(kUpdate), kSource));
    session.Close();
    EXPECT_EQ(session.TotalRecordsWritten(), 0U);
}

TEST_F(JournalThreadDir, OrderingHoldsAcrossAReconnect) {
    // connect, frames, disconnect, connect, frames, disconnect: the sink sees
    // the sequence and each file holds exactly its own connect's records.
    CaptureSession session(Threaded(dir_));
    RecordingSink sink;
    session.AddSink(sink);

    const auto first = session.BeginConnect("connected", kSource);
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_TRUE(session.OnWireMessage(BytesOf(kSnapshot), kSource));
    EXPECT_TRUE(session.OnWireMessage(BytesOf(kUpdate), kSource));
    const auto second = session.BeginConnect("staleness watchdog", kSource);
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_TRUE(session.OnWireMessage(BytesOf(kSnapshot), kSource));
    session.Close();

    const std::vector<std::string> expected = {
        "connect 1", "frame 2", "frame 3", "disconnect 1", "connect 2", "frame 2", "disconnect 2"};
    EXPECT_EQ(sink.Events(), expected);

    const auto first_records = ReadAllRecords(*first);
    ASSERT_EQ(first_records.size(), 3U);
    EXPECT_EQ(first_records[0].payload, "connected");
    EXPECT_EQ(first_records[1].payload, kSnapshot);
    EXPECT_EQ(first_records[2].payload, kUpdate);
    const auto second_records = ReadAllRecords(*second);
    ASSERT_EQ(second_records.size(), 2U);
    EXPECT_EQ(second_records[0].payload, "staleness watchdog");
    EXPECT_EQ(second_records[0].capture_sequence, 1U);
    EXPECT_EQ(second_records[1].payload, kSnapshot);
}

TEST_F(JournalThreadDir, FileOpenFailureIsReportedSynchronouslyAtBeginConnect) {
    const std::filesystem::path blocker = dir_ / "not-a-dir";
    { std::ofstream file(blocker); }

    CaptureSession session(Threaded(blocker / "journal"));
    RecordingSink sink;
    session.AddSink(sink);
    const auto started = session.BeginConnect("connected", kSource);
    EXPECT_FALSE(started.has_value());
    EXPECT_FALSE(started.error().empty());
    EXPECT_FALSE(session.OnWireMessage(BytesOf(kUpdate), kSource));
    session.Close();
    EXPECT_TRUE(sink.Events().empty());
}

TEST_F(JournalThreadDir, OverflowLatchesFatalAndCallsTheHandlerOnce) {
    // A journal thread that has not started cannot drain, so the ring fills for
    // certain. Everything accepted before the overflow still reaches the file.
    FatalRecorder fatal;
    JournalThread journal(
        {.ring_events = kSmallRing, .on_fatal = fatal.Callback(), .start = false});

    CaptureStamper stamper;
    const std::filesystem::path path = dir_ / "overflow.journal";
    journal.Connect(std::make_unique<JournalWriter>(
                        path, JournalWriter::Config{.exchange = "kraken", .connect_id = 1}),
                    stamper.Stamp(BytesOf("connected"), kSource));

    std::size_t accepted = 0;
    while (accepted < kMoreThanAnyRingHolds &&
           journal.Push(stamper.Stamp(BytesOf(kUpdate), kSource))) {
        ++accepted;
    }
    ASSERT_LT(accepted, kMoreThanAnyRingHolds);
    EXPECT_GE(accepted, kSmallRing);

    EXPECT_TRUE(journal.Failed());
    EXPECT_NE(journal.Error().find("overflow"), std::string_view::npos) << journal.Error();
    EXPECT_EQ(fatal.Calls(), 1);
    // Raised on the producer's own thread, and not again for later frames.
    EXPECT_EQ(fatal.Thread(), std::this_thread::get_id());
    EXPECT_FALSE(journal.Push(stamper.Stamp(BytesOf(kUpdate), kSource)));
    EXPECT_EQ(fatal.Calls(), 1);
    EXPECT_EQ(journal.Dropped(), 1U);

    // Start it and close: the barrier still works after an overflow (control
    // events are never dropped) and the file is a clean prefix of the stream.
    journal.Start();
    EXPECT_EQ(journal.Disconnect(), accepted + 1);
    EXPECT_EQ(ReadAllRecords(path).size(), accepted + 1);
    EXPECT_EQ(fatal.Calls(), 1);
}

TEST_F(JournalThreadDir, ASessionWhoseRingOverflowsCallsItsFatalHandler) {
    // Through the session: an unstarted journal thread cannot drain, so the ring
    // fills for certain on any build. Small payloads, because the sink keeps every
    // frame it sees; bounded, and it stops at the first refusal.
    CaptureSession::Config cfg = Threaded(dir_);
    cfg.journal_ring_events = 2;
    cfg.journal_start = false;
    CaptureSession session(cfg);
    FatalRecorder fatal;
    session.SetFatalHandler(fatal.Callback());
    RecordingSink sink;
    session.AddSink(sink);

    ASSERT_TRUE(session.BeginConnect("connected", kSource).has_value());
    std::size_t sent = 0;
    while (sent < kOverflowLoopBound && session.OnWireMessage(BytesOf(kUpdate), kSource)) {
        ++sent;
    }
    ASSERT_LT(sent, kOverflowLoopBound) << "the journal ring never overflowed";

    EXPECT_EQ(fatal.Calls(), 1);
    EXPECT_NE(fatal.Reason().find("overflow"), std::string::npos) << fatal.Reason();
    EXPECT_EQ(fatal.Thread(), std::this_thread::get_id());
    EXPECT_EQ(std::string(session.Error()), fatal.Reason());
    // The frame the journal refused still reached the other sinks, like a failed
    // inline write does.
    EXPECT_EQ(sink.FrameCount(), sent + 1);
    // A failed journal cannot take a new file.
    EXPECT_FALSE(session.BeginConnect("reconnected", kSource).has_value());
    session.Close();
    EXPECT_EQ(fatal.Calls(), 1);
}

TEST_F(JournalThreadDir, AFailedWriteCallsTheFatalHandlerFromTheJournalThread) {
    if (!std::filesystem::exists(kDevFull)) {
        GTEST_SKIP() << kDevFull << " is not available";
    }
    FatalRecorder fatal;
    JournalThread journal({.on_fatal = fatal.Callback()});
    CaptureStamper stamper;
    journal.Connect(std::make_unique<JournalWriter>(
                        std::filesystem::path(kDevFull),
                        JournalWriter::Config{.exchange = "kraken", .connect_id = 1}),
                    stamper.Stamp(BytesOf("connected"), kSource));

    // Bigger than the writer's buffer, so the write reaches the device.
    const std::string big(kBigPayloadBytes, 'x');
    ASSERT_TRUE(journal.Push(stamper.Stamp(BytesOf(big), kSource)));
    // The barrier is what makes the failure observable without a sleep: everything
    // queued ahead of it has been processed when it returns.
    journal.Disconnect();

    EXPECT_TRUE(journal.Failed());
    EXPECT_EQ(journal.Error(), "write failed");
    EXPECT_EQ(fatal.Calls(), 1);
    EXPECT_NE(fatal.Thread(), std::this_thread::get_id());
    EXPECT_FALSE(journal.Push(stamper.Stamp(BytesOf(kUpdate), kSource)));
    EXPECT_EQ(fatal.Calls(), 1);
}

TEST_F(JournalThreadDir, AFailedFlushAtCloseCallsTheFatalHandler) {
    if (!std::filesystem::exists(kDevFull)) {
        GTEST_SKIP() << kDevFull << " is not available";
    }
    FatalRecorder fatal;
    JournalThread journal({.on_fatal = fatal.Callback()});
    CaptureStamper stamper;
    journal.Connect(std::make_unique<JournalWriter>(
                        std::filesystem::path(kDevFull),
                        JournalWriter::Config{.exchange = "kraken", .connect_id = 1}),
                    stamper.Stamp(BytesOf("connected"), kSource));
    ASSERT_TRUE(journal.Push(stamper.Stamp(BytesOf(kUpdate), kSource)));
    // Everything so far fits the writer's buffer: only the flush reaches the device.
    journal.Disconnect();

    EXPECT_TRUE(journal.Failed());
    EXPECT_EQ(journal.Error(), "flush failed");
    EXPECT_EQ(fatal.Calls(), 1);
    EXPECT_NE(fatal.Thread(), std::this_thread::get_id());
}
