// The live book path without a socket: a ring and its sink on the producer side,
// the book thread on the consumer side. Frames come from the two committed real
// captures (a Kraken level3 journal and a Deribit FIX slice), so "the live path
// gives the replay's numbers" is checked against real wire data, not a fake.
//
// None of these tests depends on timing. Where an exact interleaving matters the
// consumer is left unstarted and driven with BookService::Poll(), or the ring is
// filled while nothing drains it; where the consumer runs concurrently, the
// producers are joined and Stop() drains before anything is asserted. Payloads
// stay small and every loop is bounded, so a test that goes wrong cannot run away
// with memory.
#include "book_adapter/book_service.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "book_adapter/book_adapter.h"
#include "book_adapter/book_settings.h"
#include "book_adapter/journal_replay.h"
#include "book_adapter/ring_sink.h"
#include "feed_handler/capture_session.h"
#include "feed_handler/feed_event.h"
#include "feed_handler/journal_format.h"
#include "feed_handler/journal_reader.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/test_support.h"
#include "order_book/instrument_scale.h"
#include "order_book/types.h"

namespace book_adapter {
namespace {

using feed_handler::CaptureFrame;
using feed_handler::FrameSource;
using order_book::Readiness;

// The settings of the committed captures (kraken_capture_replay_test and
// deribit_fix_replay_test document where each number comes from).
constexpr order_book::InstrumentScale kKrakenScale(/*price_decimals=*/1, /*quantity_decimals=*/8);
constexpr order_book::InstrumentScale kDeribitScale(/*price_decimals=*/1, /*quantity_decimals=*/0);
constexpr std::size_t kKrakenDepth = 10;
constexpr std::string_view kKrakenSymbol = "BTC/USD";
constexpr std::string_view kDeribitSymbol = "BTC-PERPETUAL";

// Room for every event of a fixture, so a test that expects no drops cannot get
// one however slow the consumer is.
constexpr std::size_t kRoomyRing = 1U << 15U;
// A ring small enough to fill after a handful of pushes.
constexpr std::size_t kTinyRing = 8;
// How many fixture frames a test that fills a ring offers it: far more than the
// tiny ring holds, few enough to stay cheap.
constexpr std::size_t kOfferedFrames = 200;
// How many book frames a test that needs "a snapshot and a few updates" takes.
constexpr std::size_t kSomeFrames = 4;

const BookSettings kKrakenSettings{
    .source = FrameSource::kKrakenJson,
    .scale = kKrakenScale,
    .kraken_depth = kKrakenDepth,
};
const BookSettings kDeribitSettings{
    .source = FrameSource::kDeribitFix,
    .scale = kDeribitScale,
};

std::filesystem::path FixturePath(std::string_view name) {
    return std::filesystem::path(TEST_DATA_DIR) / std::string(name);
}

std::span<const std::byte> BytesOf(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string TextOf(std::span<const std::byte> bytes) {
    std::string text(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(),
                           [](std::byte byte) { return static_cast<char>(byte); });
    return text;
}

// Every wire message of a journal, as text. Empty (and a failed test) when the
// file cannot be read.
std::vector<std::string> WireMessages(const std::filesystem::path& path) {
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

// The Kraken level3 messages from the first snapshot on: a snapshot followed by
// the updates that continue it, in order, so any prefix of the result applies
// cleanly to a fresh book.
std::vector<std::string> KrakenBookFrames() {
    std::vector<std::string> frames;
    for (std::string& message : WireMessages(FixturePath("kraken_l3_capture.journal"))) {
        if (message.find(R"("channel":"level3")") == std::string::npos) {
            continue;
        }
        if (frames.empty() && message.find(R"("type":"snapshot")") == std::string::npos) {
            continue;
        }
        frames.push_back(std::move(message));
    }
    return frames;
}

CaptureFrame FrameOf(const std::string& payload, std::uint64_t sequence) {
    return CaptureFrame{.payload = BytesOf(payload), .capture_sequence = sequence};
}

// Pushes `frames` through `sink` the way a session would: a connect, the frames,
// and (when asked) the disconnect.
void Produce(feed_handler::MessageSink& sink, const std::vector<std::string>& frames,
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

// Produce() on a thread of its own, awaited: a real connection's producer is never
// the thread that consumes.
void ProduceOnAThread(feed_handler::MessageSink& sink, const std::vector<std::string>& frames,
                      std::uint64_t connect_id) {
    std::thread producer([&] { Produce(sink, frames, connect_id, /*disconnect=*/true); });
    producer.join();
}

// Offers `frames` to `sink` until the (unstarted) service's ring for `index` has
// refused something, then returns how many it refused. Fails the test if the
// offered frames never filled it.
std::uint64_t FillUntilDropped(feed_handler::MessageSink& sink, const BookService& service,
                               std::size_t index, const std::vector<std::string>& frames) {
    std::uint64_t sequence = 0;
    for (const std::string& frame : frames) {
        sink.OnFrame(FrameOf(frame, ++sequence));
        if (service.Dropped(index) > 0) {
            break;
        }
    }
    EXPECT_GT(service.Dropped(index), 0U) << "the ring never filled";
    return service.Dropped(index);
}

std::vector<std::string> FirstFrames(const std::vector<std::string>& frames, std::size_t count) {
    return {frames.begin(),
            frames.begin() + static_cast<std::ptrdiff_t>(std::min(count, frames.size()))};
}

// ---------------------------------------------------------------------------
// RingSink: the producer half.
// ---------------------------------------------------------------------------

RingEntry MustPop(BookRing& ring) {
    RingEntry entry;
    EXPECT_TRUE(ring.Pop(entry)) << "the ring was empty";
    return entry;
}

// Offers "frame" to `sink` until the ring has refused something, and returns how
// many it offered. Fails the test if the ring never filled.
std::uint64_t FillRing(RingSink& sink, const BookRing& ring) {
    std::uint64_t offered = 0;
    while (ring.Dropped() == 0 && offered < kOfferedFrames) {
        sink.OnFrame(FrameOf("frame", ++offered));
    }
    EXPECT_GT(ring.Dropped(), 0U) << "the ring never filled";
    return offered;
}

struct EventKinds {
    std::size_t frames = 0;
    std::size_t connects = 0;
    std::size_t disconnects = 0;
};

// Empties the ring, counting what came out by kind.
EventKinds Drain(BookRing& ring) {
    EventKinds kinds;
    RingEntry entry;
    while (ring.Pop(entry)) {
        kinds.frames += std::holds_alternative<feed_handler::FrameEvent>(entry.event) ? 1U : 0U;
        kinds.connects += std::holds_alternative<feed_handler::ConnectEvent>(entry.event) ? 1U : 0U;
        kinds.disconnects +=
            std::holds_alternative<feed_handler::DisconnectEvent>(entry.event) ? 1U : 0U;
    }
    return kinds;
}

// The Kraken book of `symbol` on the service's `index`th connection, or null.
const KrakenBook* KrakenBookOf(const BookService& service, std::size_t index) {
    return service.Adapter().FindKrakenBook(service.Handle(index), kKrakenSymbol);
}

const ConnectionStats& StatsOf(const BookService& service, std::size_t index) {
    return service.Adapter().Stats(service.Handle(index));
}

TEST(RingSink, CopiesThePayloadSoTheProducerMayReuseItsBufferAtOnce) {
    BookRing ring(kTinyRing);
    RingSink sink(ring);

    std::vector<std::byte> buffer(BytesOf("original payload").begin(),
                                  BytesOf("original payload").end());
    sink.OnFrame(CaptureFrame{.payload = buffer,
                              .capture_sequence = 7,
                              .monotonic_ns = 99,
                              .source = FrameSource::kKrakenJson});
    // What a client does the instant OnFrame returns: reuse the memory.
    std::ranges::fill(buffer, std::byte{0xFF});
    buffer.clear();
    buffer.shrink_to_fit();

    const RingEntry entry = MustPop(ring);
    const auto* frame = std::get_if<feed_handler::FrameEvent>(&entry.event);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(TextOf(frame->payload), "original payload");
    EXPECT_EQ(frame->capture_sequence, 7U);
    EXPECT_EQ(frame->monotonic_ns, 99U);
    EXPECT_EQ(frame->source, FrameSource::kKrakenJson);
}

TEST(RingSink, ConnectAndDisconnectCarryTheirFieldsInOrder) {
    BookRing ring(kTinyRing);
    RingSink sink(ring);
    sink.OnConnect(3, "reconnect");
    sink.OnFrame(FrameOf("x", 1));
    sink.OnDisconnect(3);

    const RingEntry connect = MustPop(ring);
    const auto* connected = std::get_if<feed_handler::ConnectEvent>(&connect.event);
    ASSERT_NE(connected, nullptr);
    EXPECT_EQ(connected->connect_id, 3U);
    EXPECT_EQ(connected->reason, "reconnect");
    const RingEntry frame = MustPop(ring);
    EXPECT_TRUE(std::holds_alternative<feed_handler::FrameEvent>(frame.event));
    const RingEntry disconnect = MustPop(ring);
    const auto* disconnected = std::get_if<feed_handler::DisconnectEvent>(&disconnect.event);
    ASSERT_NE(disconnected, nullptr);
    EXPECT_EQ(disconnected->connect_id, 3U);
}

TEST(RingSink, AFullRingDropsFramesAndCountsThemButNeverDropsControlEvents) {
    BookRing ring(kTinyRing);
    RingSink sink(ring);
    const std::uint64_t offered = FillRing(sink, ring);
    const std::uint64_t dropped = ring.Dropped();

    // The ring is full: control events still go in, and are not counted as drops.
    sink.OnDisconnect(1);
    sink.OnConnect(2, "again");
    EXPECT_EQ(ring.Dropped(), dropped);

    // Every accepted frame, then both control events.
    const EventKinds kinds = Drain(ring);
    EXPECT_EQ(kinds.frames + dropped, offered);
    EXPECT_EQ(kinds.connects, 1U);
    EXPECT_EQ(kinds.disconnects, 1U);
}

TEST(RingSink, EveryEntryCarriesTheDropsThatPrecedeItInTheStream) {
    BookRing ring(kTinyRing);
    RingSink sink(ring);
    FillRing(sink, ring);
    const std::uint64_t dropped = ring.Dropped();

    // Everything accepted so far was pushed before the first drop.
    RingEntry entry;
    while (ring.Pop(entry)) {
        EXPECT_EQ(entry.dropped_before, 0U);
    }
    // Room again; what is pushed now comes after the drops.
    sink.OnFrame(FrameOf("after", 1));
    sink.OnDisconnect(1);
    EXPECT_EQ(MustPop(ring).dropped_before, dropped);
    EXPECT_EQ(MustPop(ring).dropped_before, dropped);
}

// ---------------------------------------------------------------------------
// BookService: the consumer half, and the two together.
// ---------------------------------------------------------------------------

TEST(BookService, ARunWithNoConnectionsStartsNoThread) {
    BookService service(BookService::Config{});
    service.Start();
    EXPECT_FALSE(service.Running());
    EXPECT_EQ(service.ConnectionCount(), 0U);
    service.Stop();
    EXPECT_FALSE(service.Running());
}

TEST(BookService, StartCreatesTheThreadAndStopEndsItAndBothAreIdempotent) {
    BookService service(BookService::Config{.ring_events = kTinyRing});
    service.AddConnection("kraken-a", kKrakenSettings);
    service.Start();
    EXPECT_TRUE(service.Running());
    service.Start();  // A second Start is not a second thread.
    service.Stop();
    EXPECT_FALSE(service.Running());
    service.Stop();
    service.Start();  // A stopped service does not restart.
    EXPECT_FALSE(service.Running());
}

TEST(BookService, StopDrainsEverythingQueuedEvenWhenTheThreadNeverStarted) {
    const std::vector<std::string> frames = FirstFrames(KrakenBookFrames(), kSomeFrames);
    ASSERT_EQ(frames.size(), kSomeFrames);
    BookService service(BookService::Config{.ring_events = kRoomyRing});
    Produce(service.AddConnection("kraken-a", kKrakenSettings), frames, 1, /*disconnect=*/true);

    service.Stop();

    const ConnectionStats& stats = StatsOf(service, 0);
    EXPECT_EQ(stats.frames, kSomeFrames);
    EXPECT_EQ(stats.snapshots, 1U);
    EXPECT_EQ(stats.updates, kSomeFrames - 1);
    EXPECT_EQ(stats.connects, 1U);
    EXPECT_EQ(stats.disconnects, 1U);
    EXPECT_EQ(stats.drops, 0U);
}

TEST(BookService, ARunningConsumerDrainsWhatWasPushedBeforeStop) {
    const std::vector<std::string> frames = FirstFrames(KrakenBookFrames(), kOfferedFrames);
    ASSERT_GT(frames.size(), kSomeFrames);
    BookService service(BookService::Config{.ring_events = kRoomyRing});
    RingSink& sink = service.AddConnection("kraken-a", kKrakenSettings);
    service.Start();
    ProduceOnAThread(sink, frames, 1);
    service.Stop();

    const ConnectionStats& stats = StatsOf(service, 0);
    EXPECT_EQ(stats.frames, frames.size());
    EXPECT_EQ(stats.connects, 1U);
    EXPECT_EQ(stats.disconnects, 1U);
    EXPECT_EQ(stats.drops, 0U);
    ASSERT_NE(KrakenBookOf(service, 0), nullptr);
    EXPECT_EQ(KrakenBookOf(service, 0)->GetReadiness(), Readiness::kReady);
}

// The strongest check of the whole path: every frame of the real Kraken and the
// real Deribit capture goes through its own ring, from its own producer thread,
// into one book thread, and the adapter ends up with the numbers a plain inline
// replay of the same files gives.
TEST(BookService, LiveRingsGiveTheSameCountsAsAnInlineReplayOfTheSameJournals) {
    const std::vector<std::string> kraken = WireMessages(FixturePath("kraken_l3_capture.journal"));
    const std::vector<std::string> deribit =
        WireMessages(FixturePath("deribit_fix_capture.journal"));
    ASSERT_FALSE(kraken.empty());
    ASSERT_FALSE(deribit.empty());

    JournalReplay kraken_replay(ReplayOptions{.scale = kKrakenScale, .kraken_depth = kKrakenDepth});
    JournalReplay deribit_replay(ReplayOptions{.scale = kDeribitScale});
    const std::vector<std::filesystem::path> kraken_path{FixturePath("kraken_l3_capture.journal")};
    const std::vector<std::filesystem::path> deribit_path{
        FixturePath("deribit_fix_capture.journal")};
    const auto kraken_report = kraken_replay.Run(kraken_path);
    const auto deribit_report = deribit_replay.Run(deribit_path);
    ASSERT_TRUE(kraken_report.has_value());
    ASSERT_TRUE(deribit_report.has_value());

    BookService service(BookService::Config{.ring_events = kRoomyRing});
    RingSink& kraken_sink = service.AddConnection("kraken-a", kKrakenSettings);
    RingSink& deribit_sink = service.AddConnection("deribit-a", kDeribitSettings);
    service.Start();
    std::thread kraken_producer([&] { Produce(kraken_sink, kraken, 1, /*disconnect=*/true); });
    std::thread deribit_producer([&] { Produce(deribit_sink, deribit, 1, /*disconnect=*/true); });
    kraken_producer.join();
    deribit_producer.join();
    service.Stop();

    ASSERT_EQ(service.Dropped(0), 0U) << "the ring was meant to hold the whole fixture";
    ASSERT_EQ(service.Dropped(1), 0U) << "the ring was meant to hold the whole fixture";
    const std::vector<const ConnectionStats*> live = {&StatsOf(service, 0),
                                                      &service.Adapter().Stats(service.Handle(1))};
    const std::vector<const ConnectionStats*> replayed = {&kraken_report->stats,
                                                          &deribit_report->stats};
    for (std::size_t index = 0; index < live.size(); ++index) {
        SCOPED_TRACE(index == 0 ? "kraken" : "deribit");
        EXPECT_EQ(live[index]->frames, replayed[index]->frames);
        EXPECT_EQ(live[index]->snapshots, replayed[index]->snapshots);
        EXPECT_EQ(live[index]->updates, replayed[index]->updates);
        EXPECT_EQ(live[index]->updates_before_snapshot, replayed[index]->updates_before_snapshot);
        EXPECT_EQ(live[index]->updates_while_desynced, replayed[index]->updates_while_desynced);
        EXPECT_EQ(live[index]->TotalIssues(), replayed[index]->TotalIssues());
        EXPECT_EQ(live[index]->parse_errors, replayed[index]->parse_errors);
        EXPECT_EQ(live[index]->apply_errors, replayed[index]->apply_errors);
        EXPECT_EQ(live[index]->connects, replayed[index]->connects);
        EXPECT_EQ(live[index]->disconnects, replayed[index]->disconnects);
        EXPECT_EQ(live[index]->drops, 0U);
    }
    EXPECT_EQ(service.Adapter().BookCount(service.Handle(0)), 1U);
    EXPECT_EQ(service.Adapter().BookCount(service.Handle(1)), 1U);
    const KrakenBook* kraken_book =
        service.Adapter().FindKrakenBook(service.Handle(0), kKrakenSymbol);
    const DeribitBook* deribit_book =
        service.Adapter().FindDeribitBook(service.Handle(1), kDeribitSymbol);
    ASSERT_NE(kraken_book, nullptr);
    ASSERT_NE(deribit_book, nullptr);
    EXPECT_EQ(kraken_book->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(deribit_book->GetReadiness(), Readiness::kReady);
}

TEST(BookService, AFullRingDropsFramesAndTheBookStaysDesyncedUntilTheNextSnapshot) {
    const std::vector<std::string> frames = FirstFrames(KrakenBookFrames(), kOfferedFrames);
    ASSERT_GT(frames.size(), kTinyRing);
    // The consumer is not started, so nothing drains the ring: it fills for sure.
    BookService service(BookService::Config{.ring_events = kTinyRing});
    RingSink& sink = service.AddConnection("kraken-a", kKrakenSettings);
    sink.OnConnect(1, "test");
    const std::uint64_t dropped = FillUntilDropped(sink, service, 0, frames);

    service.Start();
    service.Stop();

    const ConnectionStats& stats = StatsOf(service, 0);
    EXPECT_EQ(stats.drops, dropped);
    EXPECT_EQ(stats.snapshots, 1U);
    const KrakenBook* book = KrakenBookOf(service, 0);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->GetReadiness(), Readiness::kDesynced);
}

// The drop that a plain "compare the live counter after each pop" would attribute
// to the wrong connection. The ring fills after Connect 2 was pushed, while
// Connect 2 is still queued; the consumer must not learn of the drop before it
// has reset the books for Connect 2, or the reset would wipe the desync and the
// new book would silently be missing a frame.
TEST(BookService, ADropAfterAConnectStillDesyncsThatConnectsBook) {
    const std::vector<std::string> frames = FirstFrames(KrakenBookFrames(), kOfferedFrames);
    ASSERT_GT(frames.size(), kTinyRing);
    BookService service(BookService::Config{.ring_events = kTinyRing});
    RingSink& sink = service.AddConnection("kraken-a", kKrakenSettings);

    sink.OnConnect(1, "first");
    sink.OnFrame(FrameOf(frames[0], 1));
    ASSERT_GT(service.Poll(), 0U);
    sink.OnDisconnect(1);
    sink.OnConnect(2, "second");
    const std::uint64_t dropped = FillUntilDropped(sink, service, 0, frames);

    // One consumer pass sees Connect 2 and the frames behind it with the drop
    // already counted in the ring.
    ASSERT_GT(service.Poll(), 0U);
    service.Stop();

    const ConnectionStats& stats = StatsOf(service, 0);
    EXPECT_EQ(stats.drops, dropped);
    EXPECT_EQ(service.Adapter().ConnectId(service.Handle(0)), 2U);
    const KrakenBook* book = KrakenBookOf(service, 0);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->GetReadiness(), Readiness::kDesynced);
}

TEST(BookService, ADropBeforeAConnectDoesNotPoisonTheNewConnectionsBook) {
    const std::vector<std::string> frames = FirstFrames(KrakenBookFrames(), kOfferedFrames);
    ASSERT_GT(frames.size(), kTinyRing);
    BookService service(BookService::Config{.ring_events = kTinyRing});
    RingSink& sink = service.AddConnection("kraken-a", kKrakenSettings);

    sink.OnConnect(1, "first");
    const std::uint64_t dropped = FillUntilDropped(sink, service, 0, frames);
    ASSERT_GT(service.Poll(), 0U);

    // The connection ends and a new one starts: the drops belong to the old one.
    sink.OnDisconnect(1);
    Produce(sink, FirstFrames(frames, kSomeFrames), 2, /*disconnect=*/false);
    ASSERT_GT(service.Poll(), 0U);
    service.Stop();

    const ConnectionStats& stats = StatsOf(service, 0);
    EXPECT_EQ(stats.drops, dropped);
    EXPECT_EQ(service.Dropped(0), dropped) << "the new connection lost a frame too";
    const KrakenBook* book = KrakenBookOf(service, 0);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);
}

TEST(BookService, ASessionFeedsTheServiceThroughAddSinkFromConnectToDisconnect) {
    const std::vector<std::string> frames = FirstFrames(KrakenBookFrames(), kSomeFrames);
    ASSERT_EQ(frames.size(), kSomeFrames);
    const std::filesystem::path dir = feed_handler::test_support::UniqueTestDir("book_service");
    {
        BookService service(BookService::Config{.ring_events = kRoomyRing});
        feed_handler::CaptureSession session({.directory = dir, .exchange = "kraken"});
        session.AddSink(service.AddConnection("kraken-a", kKrakenSettings));

        ASSERT_TRUE(session.BeginConnect("test", FrameSource::kKrakenJson).has_value());
        for (const std::string& frame : frames) {
            ASSERT_TRUE(session.OnWireMessage(BytesOf(frame), FrameSource::kKrakenJson));
        }
        session.Close();
        service.Stop();

        const ConnectionStats& stats = StatsOf(service, 0);
        EXPECT_EQ(stats.connects, 1U);
        EXPECT_EQ(stats.frames, kSomeFrames);
        EXPECT_EQ(stats.snapshots, 1U);
        EXPECT_EQ(stats.disconnects, 1U);
        EXPECT_TRUE(service.Adapter().IsStale(service.Handle(0)));
    }
    std::filesystem::remove_all(dir);
}

TEST(BookService, ASummaryNamesTheConnectionAndCountsWhatItSaw) {
    const std::vector<std::string> frames = FirstFrames(KrakenBookFrames(), kSomeFrames);
    ASSERT_EQ(frames.size(), kSomeFrames);
    BookService service(BookService::Config{.ring_events = kRoomyRing});
    Produce(service.AddConnection("kraken-a", kKrakenSettings), frames, 1, /*disconnect=*/false);
    service.Stop();

    const std::string summary = service.Summary(0);
    EXPECT_NE(summary.find("[kraken-a]"), std::string::npos) << summary;
    EXPECT_NE(summary.find("4 frames"), std::string::npos) << summary;
    EXPECT_NE(summary.find("1 snapshots"), std::string::npos) << summary;
    EXPECT_NE(summary.find("3 updates"), std::string::npos) << summary;
    EXPECT_NE(summary.find("0 dropped"), std::string::npos) << summary;
    EXPECT_NE(summary.find("0 parse errors"), std::string::npos) << summary;
    EXPECT_NE(summary.find("integrity issues: none"), std::string::npos) << summary;
    EXPECT_NE(summary.find("1 book(s)"), std::string::npos) << summary;
}

TEST(BookService, AnUnparsableFrameIsCountedAndNeverEscapesTheThread) {
    BookService service(BookService::Config{.ring_events = kTinyRing});
    RingSink& sink = service.AddConnection("kraken-a", kKrakenSettings);
    service.Start();
    std::thread producer([&] {
        sink.OnConnect(1, "test");
        sink.OnFrame(FrameOf("this is not json", 1));
        sink.OnDisconnect(1);
    });
    producer.join();
    service.Stop();

    const ConnectionStats& stats = StatsOf(service, 0);
    EXPECT_EQ(stats.frames, 1U);
    EXPECT_EQ(stats.parse_errors, 1U);
    EXPECT_EQ(stats.disconnects, 1U);
}

}  // namespace
}  // namespace book_adapter
