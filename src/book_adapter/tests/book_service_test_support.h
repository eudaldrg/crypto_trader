// Shared by the ring-overflow and concurrency tests of the live book path: the
// settings of the two committed real captures, the frames read back from them, and
// an inline oracle (a plain BookAdapter driven on the calling thread) whose result
// a threaded run has to match.
//
// Everything here is bounded on purpose. Frames come from the committed fixtures,
// so their sizes are the wire's; the tests take a fixed few hundred of them and
// nothing here retains more than that.
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "book_adapter/book_adapter.h"
#include "book_adapter/book_service.h"
#include "book_adapter/book_settings.h"
#include "feed_handler/journal_format.h"
#include "feed_handler/journal_reader.h"
#include "feed_handler/message_sink.h"
#include "order_book/instrument_scale.h"
#include "order_book/types.h"

namespace book_adapter::test_support {

inline constexpr order_book::InstrumentScale kKrakenScale(/*price_decimals=*/1,
                                                          /*quantity_decimals=*/8);
inline constexpr order_book::InstrumentScale kDeribitScale(/*price_decimals=*/1,
                                                           /*quantity_decimals=*/0);
inline constexpr std::size_t kKrakenDepth = 10;
inline constexpr std::string_view kKrakenSymbol = "BTC/USD";
inline constexpr std::string_view kDeribitSymbol = "BTC-PERPETUAL";

/// A ring small enough to fill after a handful of pushes.
inline constexpr std::size_t kTinyRing = 8;
/// A ring that holds every frame any of these tests offers, so a test that
/// expects no drops cannot get one however slow the consumer is.
inline constexpr std::size_t kRoomyRing = 1U << 10U;
/// How many fixture frames a test offers: far more than the tiny ring holds, few
/// enough to stay cheap (a few hundred small messages, some tens of KB).
inline constexpr std::size_t kOfferedFrames = 300;
/// "A snapshot and a few updates".
inline constexpr std::size_t kSomeFrames = 4;
/// The most a producer is given to finish a fixed loop that cannot block. Far
/// beyond what it takes (microseconds): a bound only a real hang can hit.
inline constexpr std::chrono::seconds kProducerBound{60};

inline const BookSettings kKrakenSettings{
    .source = feed_handler::FrameSource::kKrakenJson,
    .scale = kKrakenScale,
    .kraken_depth = kKrakenDepth,
};
inline const BookSettings kDeribitSettings{
    .source = feed_handler::FrameSource::kDeribitFix,
    .scale = kDeribitScale,
};

inline std::filesystem::path FixturePath(std::string_view name) {
    return std::filesystem::path(TEST_DATA_DIR) / std::string(name);
}

inline std::span<const std::byte> BytesOf(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

inline std::string TextOf(std::span<const std::byte> bytes) {
    std::string text(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(),
                           [](std::byte byte) { return static_cast<char>(byte); });
    return text;
}

/// Every wire message of a journal, as text. Empty (and a failed test) when the
/// file cannot be read.
inline std::vector<std::string> WireMessages(const std::filesystem::path& path) {
    std::vector<std::string> messages;
    auto reader = feed_handler::JournalReader::Open(path);
    EXPECT_TRUE(reader.has_value()) << "missing capture fixture: " << path;
    if (!reader) {
        return messages;
    }
    while (const std::optional<feed_handler::JournalRecord> record = reader->Next()) {
        if (record->type == feed_handler::journal::RecordType::kWireMessage) {
            messages.push_back(TextOf(record->payload));
        }
    }
    EXPECT_FALSE(reader->StoppedEarly()) << "fixture journal is corrupt or truncated";
    return messages;
}

/// The first `count` frames (fewer if there are fewer).
inline std::vector<std::string> FirstFrames(const std::vector<std::string>& frames,
                                            std::size_t count) {
    return {frames.begin(),
            frames.begin() + static_cast<std::ptrdiff_t>(std::min(count, frames.size()))};
}

/// The Kraken level3 messages from the first snapshot on: a snapshot followed by
/// the updates that continue it, in order, so any prefix of the result applies
/// cleanly to a fresh book. At most kOfferedFrames of them.
inline std::vector<std::string> KrakenBookFrames() {
    std::vector<std::string> frames;
    for (std::string& message : WireMessages(FixturePath("kraken_l3_capture.journal"))) {
        if (message.find(R"("channel":"level3")") == std::string::npos) {
            continue;
        }
        if (frames.empty() && message.find(R"("type":"snapshot")") == std::string::npos) {
            continue;
        }
        frames.push_back(std::move(message));
        if (frames.size() == kOfferedFrames) {
            break;
        }
    }
    return frames;
}

/// The first kOfferedFrames Deribit FIX messages of the committed slice, in order.
inline std::vector<std::string> DeribitFrames() {
    return FirstFrames(WireMessages(FixturePath("deribit_fix_capture.journal")), kOfferedFrames);
}

inline feed_handler::CaptureFrame FrameOf(const std::string& payload, std::uint64_t sequence) {
    return feed_handler::CaptureFrame{.payload = BytesOf(payload), .capture_sequence = sequence};
}

/// Pushes `frames` through `sink` the way a session would: a connect, the frames
/// (sequence numbers from 1) and, when asked, the disconnect.
inline void Produce(feed_handler::MessageSink& sink, const std::vector<std::string>& frames,
                    std::uint64_t connect_id, bool disconnect) {
    sink.OnConnect(connect_id, "test");
    std::uint64_t sequence = 0;
    for (const std::string& frame : frames) {
        sink.OnFrame(FrameOf(frame, ++sequence));
    }
    if (disconnect) {
        sink.OnDisconnect(connect_id);
    }
}

/// Runs `work` on a thread of its own and waits for it, but never longer than
/// `bound`: a producer that must not block is checked by it finishing, and a
/// producer that did block fails the test here instead of hanging the run. If the
/// bound passes, `unblock` is called to free the thread (start the consumer that
/// was being outrun) so it can be joined. Returns whether `work` finished in time.
template <typename Work, typename Unblock>
bool RunWithin(std::chrono::seconds bound, const Work& work, const Unblock& unblock) {
    std::promise<void> finished;
    std::future<void> done = finished.get_future();
    std::thread producer([&] {
        work();
        finished.set_value();
    });
    const bool in_time = done.wait_for(bound) == std::future_status::ready;
    if (!in_time) {
        unblock();
    }
    producer.join();
    return in_time;
}

/// The result a threaded run must reproduce: one connection driven inline, on the
/// calling thread, with the same events in the same order.
class InlineOracle {
  public:
    explicit InlineOracle(const BookSettings& settings)
        : handle_(adapter_.AddConnection("oracle", settings)) {}

    void Connect(std::uint64_t connect_id) {
        adapter_.OnConnect(handle_, connect_id, "test");
    }

    void Disconnect(std::uint64_t connect_id) {
        adapter_.OnDisconnect(handle_, connect_id);
    }

    void Frames(const std::vector<std::string>& frames) {
        std::uint64_t sequence = 0;
        for (const std::string& frame : frames) {
            adapter_.OnFrame(handle_, FrameOf(frame, ++sequence));
        }
    }

    [[nodiscard]] const BookAdapter& Adapter() const {
        return adapter_;
    }
    [[nodiscard]] ConnectionHandle Handle() const {
        return handle_;
    }
    [[nodiscard]] const ConnectionStats& Stats() const {
        return adapter_.Stats(handle_);
    }

  private:
    BookAdapter adapter_;
    ConnectionHandle handle_;
};

/// Every counter a book-affecting event can move, in a fixed order, so two runs
/// compare with one assertion: frames, snapshots, updates, updates_before_snapshot,
/// updates_while_desynced, frames_ignored_stale, parse_errors, apply_errors, drops,
/// connects, disconnects, and the integrity issues of every kind added up.
inline std::array<std::uint64_t, 12> CountsOf(const ConnectionStats& stats) {
    return {stats.frames,
            stats.snapshots,
            stats.updates,
            stats.updates_before_snapshot,
            stats.updates_while_desynced,
            stats.frames_ignored_stale,
            stats.parse_errors,
            stats.apply_errors,
            stats.drops,
            stats.connects,
            stats.disconnects,
            stats.TotalIssues()};
}

inline void ExpectSameCounts(const ConnectionStats& live, const ConnectionStats& expected) {
    EXPECT_EQ(CountsOf(live), CountsOf(expected));
}

/// The same book on both sides, as far as its readiness and best prices tell.
template <typename Book>
void ExpectSameBook(const Book* live, const Book* expected) {
    ASSERT_NE(live, nullptr);
    ASSERT_NE(expected, nullptr);
    EXPECT_EQ(live->GetReadiness(), expected->GetReadiness());
    EXPECT_EQ(live->Best(order_book::Side::kBid), expected->Best(order_book::Side::kBid));
    EXPECT_EQ(live->Best(order_book::Side::kAsk), expected->Best(order_book::Side::kAsk));
}

/// The Kraken book of the fixture's symbol on the service's `index`th connection.
inline const KrakenBook* KrakenBookOf(const BookService& service, std::size_t index) {
    return service.Adapter().FindKrakenBook(service.Handle(index), kKrakenSymbol);
}

inline const ConnectionStats& StatsOf(const BookService& service, std::size_t index) {
    return service.Adapter().Stats(service.Handle(index));
}

}  // namespace book_adapter::test_support
