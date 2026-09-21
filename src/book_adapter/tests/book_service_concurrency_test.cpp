// Several producer threads and one book thread at once: each connection's events
// stay in the order its producer sent them, and one connection's events, drops or
// reconnects never reach another connection's books.
//
// The consumer runs concurrently here, so nothing asserts an interleaving. What is
// asserted is only what has to hold for every interleaving, after the producers
// are joined and Stop() has drained: exact counters and book state against an
// inline oracle fed the same events, and (for order) that a real feed's checksums,
// which break on any reordered or foreign update, all pass. Rings are sized above
// what a test pushes, so no frame can be dropped whatever the scheduler does, and
// payloads are the small fixture frames: a bounded loop, bounded memory
// (docs/tasks/testing.md).
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "book_adapter/book_service.h"
#include "book_adapter/ring_sink.h"
#include "book_adapter/tests/book_service_test_support.h"
#include "feed_handler/capture_session.h"
#include "feed_handler/feed_event.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/test_support.h"
#include "order_book/types.h"

namespace book_adapter {
namespace {

using feed_handler::CaptureSession;
using feed_handler::FrameSource;
using feed_handler::JournalMode;
using order_book::Readiness;
using test_support::BytesOf;
using test_support::ExpectSameBook;
using test_support::ExpectSameCounts;
using test_support::FirstFrames;
using test_support::FrameOf;
using test_support::InlineOracle;
using test_support::kDeribitSettings;
using test_support::kDeribitSymbol;
using test_support::kKrakenSettings;
using test_support::kKrakenSymbol;
using test_support::KrakenBookFrames;
using test_support::KrakenBookOf;
using test_support::kRoomyRing;
using test_support::Produce;
using test_support::StatsOf;

// Rounds of the whole scenario, each with a fresh service: more interleavings for
// a repeated run to catch, still a few hundred small frames in all.
constexpr std::size_t kRounds = 3;

// Producers start together, so their pushes overlap instead of running one after
// the other.
class StartingGate {
  public:
    void Open() {
        open_.store(true, std::memory_order_release);
    }
    void Wait() const {
        while (!open_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

  private:
    std::atomic<bool> open_{false};
};

// A producer thread's body.
template <typename Body>
std::thread Spawn(const StartingGate& gate, Body body) {
    return std::thread([&gate, body = std::move(body)]() mutable {
        gate.Wait();
        body();
    });
}

std::vector<std::string> LongKrakenRun() {
    std::vector<std::string> frames = KrakenBookFrames();
    EXPECT_GE(frames.size(), 100U) << "the fixture is meant to hold a few hundred book frames";
    return frames;
}

// ---------------------------------------------------------------------------
// Two producers, one book thread, real frames.
// ---------------------------------------------------------------------------

// The expected end state of one connection: a fresh oracle fed `frames` between a
// connect and a disconnect.
InlineOracle Expected(const BookSettings& settings, const std::vector<std::string>& frames) {
    InlineOracle oracle(settings);
    oracle.Connect(1);
    oracle.Frames(frames);
    oracle.Disconnect(1);
    return oracle;
}

const KrakenBook* KrakenBookOfOracle(const InlineOracle& oracle) {
    return oracle.Adapter().FindKrakenBook(oracle.Handle(), kKrakenSymbol);
}

// Two connections to the same kind of feed and the same symbol, fed different
// amounts of the same stream: the books can only be told apart by whose frames
// they got, so any frame landing on the wrong connection changes one of them.
struct KrakenPair {
    KrakenPair()
        : longer(LongKrakenRun()),
          shorter(FirstFrames(longer, longer.size() / 3)),
          expected_long(Expected(kKrakenSettings, longer)),
          expected_short(Expected(kKrakenSettings, shorter)) {}

    std::vector<std::string> longer;
    std::vector<std::string> shorter;
    InlineOracle expected_long;
    InlineOracle expected_short;
};

void RunKrakenPairRound(const KrakenPair& pair) {
    BookService service(BookService::Config{.ring_events = kRoomyRing});
    RingSink& long_sink = service.AddConnection("kraken-long", kKrakenSettings);
    RingSink& short_sink = service.AddConnection("kraken-short", kKrakenSettings);
    service.Start();

    StartingGate gate;
    std::thread long_producer =
        Spawn(gate, [&] { Produce(long_sink, pair.longer, 1, /*disconnect=*/true); });
    std::thread short_producer =
        Spawn(gate, [&] { Produce(short_sink, pair.shorter, 1, /*disconnect=*/true); });
    gate.Open();
    long_producer.join();
    short_producer.join();
    service.Stop();

    ASSERT_EQ(service.Dropped(0), 0U) << "the ring was meant to hold everything";
    ASSERT_EQ(service.Dropped(1), 0U) << "the ring was meant to hold everything";
    EXPECT_EQ(service.InternalErrors(), 0U);
    ExpectSameCounts(StatsOf(service, 0), pair.expected_long.Stats());
    ExpectSameCounts(StatsOf(service, 1), pair.expected_short.Stats());
    EXPECT_EQ(StatsOf(service, 0).frames, pair.longer.size());
    EXPECT_EQ(StatsOf(service, 1).frames, pair.shorter.size());
    ExpectSameBook(KrakenBookOf(service, 0), KrakenBookOfOracle(pair.expected_long));
    ExpectSameBook(KrakenBookOf(service, 1), KrakenBookOfOracle(pair.expected_short));
}

TEST(BookServiceConcurrency, TwoProducersKeepTwoConnectionsBooksApartAndInOrder) {
    const KrakenPair pair;
    ASSERT_NE(pair.longer.size(), pair.shorter.size());
    // The real stream applies cleanly one frame after the other, so a reordered
    // frame would break a checksum or a book invariant and show up as an issue.
    ASSERT_EQ(pair.expected_long.Stats().TotalIssues(), 0U);
    ASSERT_EQ(pair.expected_short.Stats().TotalIssues(), 0U);
    ASSERT_EQ(pair.expected_long.Stats().updates_while_desynced, 0U);

    for (std::size_t round = 0; round < kRounds; ++round) {
        SCOPED_TRACE("round " + std::to_string(round));
        RunKrakenPairRound(pair);
    }
}

// Two connections of different exchanges: a Kraken frame must never be parsed as
// FIX or the other way round, and neither connection may grow the other's kind of
// book.
struct KrakenAndDeribit {
    KrakenAndDeribit()
        : kraken(LongKrakenRun()),
          deribit(test_support::DeribitFrames()),
          expected_kraken(Expected(kKrakenSettings, kraken)),
          expected_deribit(Expected(kDeribitSettings, deribit)) {}

    std::vector<std::string> kraken;
    std::vector<std::string> deribit;
    InlineOracle expected_kraken;
    InlineOracle expected_deribit;
};

void RunKrakenAndDeribitRound(const KrakenAndDeribit& feeds) {
    BookService service(BookService::Config{.ring_events = kRoomyRing});
    RingSink& kraken_sink = service.AddConnection("kraken-a", kKrakenSettings);
    RingSink& deribit_sink = service.AddConnection("deribit-a", kDeribitSettings);
    service.Start();

    StartingGate gate;
    std::thread kraken_producer =
        Spawn(gate, [&] { Produce(kraken_sink, feeds.kraken, 1, /*disconnect=*/true); });
    std::thread deribit_producer =
        Spawn(gate, [&] { Produce(deribit_sink, feeds.deribit, 1, /*disconnect=*/true); });
    gate.Open();
    kraken_producer.join();
    deribit_producer.join();
    service.Stop();

    ASSERT_EQ(service.Dropped(0) + service.Dropped(1), 0U);
    EXPECT_EQ(service.InternalErrors(), 0U);
    ExpectSameCounts(StatsOf(service, 0), feeds.expected_kraken.Stats());
    ExpectSameCounts(StatsOf(service, 1), feeds.expected_deribit.Stats());
    EXPECT_EQ(StatsOf(service, 0).parse_errors + StatsOf(service, 1).parse_errors, 0U);

    // Each connection holds one book, of its own kind and none of the other's:
    // books on each, and a book of the other's kind on neither.
    const BookAdapter& adapter = service.Adapter();
    const std::array<std::size_t, 4> books = {
        adapter.BookCount(service.Handle(0)), adapter.BookCount(service.Handle(1)),
        adapter.FindDeribitBook(service.Handle(0), kDeribitSymbol) == nullptr ? 0U : 1U,
        adapter.FindKrakenBook(service.Handle(1), kKrakenSymbol) == nullptr ? 0U : 1U};
    EXPECT_EQ(books, (std::array<std::size_t, 4>{1, 1, 0, 0}));
    ExpectSameBook(KrakenBookOf(service, 0), KrakenBookOfOracle(feeds.expected_kraken));
    ExpectSameBook(adapter.FindDeribitBook(service.Handle(1), kDeribitSymbol),
                   feeds.expected_deribit.Adapter().FindDeribitBook(feeds.expected_deribit.Handle(),
                                                                    kDeribitSymbol));
}

TEST(BookServiceConcurrency, AKrakenAndADeribitProducerNeverContaminateEachOthersBooks) {
    const KrakenAndDeribit feeds;
    ASSERT_GT(feeds.deribit.size(), 10U);
    for (std::size_t round = 0; round < kRounds; ++round) {
        SCOPED_TRACE("round " + std::to_string(round));
        RunKrakenAndDeribitRound(feeds);
    }
}

// ---------------------------------------------------------------------------
// capture_sequence, as stamped by real sessions.
// ---------------------------------------------------------------------------

// The payload a producer sends as frame `sequence` of connect `connect_id`: it
// names its own connection, so a frame on the wrong ring is recognizable.
std::string TaggedPayload(char tag, std::uint64_t connect_id, std::uint64_t sequence) {
    return std::string(1, tag) + "/" + std::to_string(connect_id) + "/" + std::to_string(sequence);
}

// The connect marker is the first record of every connect and takes capture
// sequence 1 (journal_writer.h), so the first frame after a connect is number 2.
constexpr std::uint64_t kMarkerSequence = 1;

// What the consumer side of one ring saw, judged as it came out. Checked on the
// consumer thread, read only after it is joined.
struct RingAudit {
    char tag = '?';
    std::uint64_t connect_id = 0;
    std::uint64_t last_sequence = 0;
    std::size_t frames = 0;
    std::size_t connects = 0;
    std::size_t disconnects = 0;
    /// A frame whose capture_sequence did not follow its predecessor's, in the
    /// same connect_id (sequence numbers restart on a connect).
    std::size_t out_of_order = 0;
    /// A frame whose payload names another connection, connect_id or sequence.
    std::size_t foreign = 0;
    /// An entry that reports drops: none can happen, the ring is never full.
    std::size_t reported_drops = 0;
    /// A frame or disconnect for a connect_id other than the open one.
    std::size_t wrong_connect = 0;

    /// Everything above in one comparable value: frames, connects, disconnects,
    /// out_of_order, foreign, wrong_connect, reported_drops, connect_id,
    /// last_sequence.
    [[nodiscard]] std::array<std::uint64_t, 9> Summary() const {
        return {frames,        connects,       disconnects, out_of_order, foreign,
                wrong_connect, reported_drops, connect_id,  last_sequence};
    }

    void Observe(const RingEntry& entry) {
        reported_drops += entry.dropped_before == 0 ? 0U : 1U;
        if (const auto* connect = std::get_if<feed_handler::ConnectEvent>(&entry.event)) {
            ++connects;
            connect_id = connect->connect_id;
            last_sequence = kMarkerSequence;
        } else if (const auto* disconnect =
                       std::get_if<feed_handler::DisconnectEvent>(&entry.event)) {
            ++disconnects;
            wrong_connect += disconnect->connect_id == connect_id ? 0U : 1U;
        } else if (const auto* frame = std::get_if<feed_handler::FrameEvent>(&entry.event)) {
            ++frames;
            out_of_order += frame->capture_sequence == last_sequence + 1 ? 0U : 1U;
            last_sequence = frame->capture_sequence;
            foreign +=
                test_support::TextOf(frame->payload) ==
                        TaggedPayload(tag, connect_id, frame->capture_sequence - kMarkerSequence)
                    ? 0U
                    : 1U;
        }
    }
};

// One consumer for two rings, popping them the way the book thread does. Returns
// once both producers are done and both rings are empty: it reads the done count
// BEFORE its last pass, so a pass that finds nothing after seeing both done has
// seen everything.
void ConsumeBoth(BookRing& ring_a, BookRing& ring_b, const std::atomic<int>& producers_done,
                 RingAudit& audit_a, RingAudit& audit_b) {
    RingEntry entry;
    for (;;) {
        const bool finished = producers_done.load(std::memory_order_acquire) == 2;
        bool popped = false;
        while (ring_a.Pop(entry)) {
            audit_a.Observe(entry);
            popped = true;
        }
        while (ring_b.Pop(entry)) {
            audit_b.Observe(entry);
            popped = true;
        }
        if (finished && !popped) {
            return;
        }
        if (!popped) {
            std::this_thread::yield();
        }
    }
}

constexpr std::uint64_t kFramesPerConnect = 250;

// RunCapture's two connects were seen whole, in order, with nothing foreign.
void ExpectCleanAudit(const RingAudit& audit) {
    SCOPED_TRACE(std::string("ring ") + audit.tag);
    const std::array<std::uint64_t, 9> expected = {
        2 * kFramesPerConnect,  // frames
        2,                      // connects
        2,                      // disconnects
        0,                      // out_of_order
        0,                      // foreign
        0,                      // wrong_connect
        0,                      // reported_drops
        2,                      // connect_id: the second connect is the open one
        kMarkerSequence + kFramesPerConnect,
    };
    EXPECT_EQ(audit.Summary(), expected);
}

// A capture as the feed handler runs one: a threaded-journal session that
// connects, streams `kFramesPerConnect` tagged frames, reconnects mid-stream (the
// counter restarts) and streams as many again. Returns how many calls failed.
std::size_t RunCapture(CaptureSession& session, char tag) {
    std::size_t failures = 0;
    for (const std::uint64_t connect_id : {std::uint64_t{1}, std::uint64_t{2}}) {
        failures += session.BeginConnect("test", FrameSource::kKrakenJson) ? 0U : 1U;
        for (std::uint64_t sequence = 1; sequence <= kFramesPerConnect; ++sequence) {
            const std::string payload = TaggedPayload(tag, connect_id, sequence);
            failures += session.OnWireMessage(BytesOf(payload), FrameSource::kKrakenJson) ? 0U : 1U;
        }
    }
    session.Close();
    return failures;
}

// The plan's order check, made where a ring's consumer sees it: the sequence of a
// session's frames in its ring is exactly what the session stamped, per connection
// and per connect_id, with two producers running at once. (The adapter keeps no
// per-frame record of capture_sequence, so this is the closest point to it; the
// two tests above cover the adapter's end of the same guarantee, since a real
// feed's checksums fail on any reordering.)
TEST(BookServiceConcurrency, CaptureSequenceIsStrictlyIncreasingPerConnectionAtTheConsumer) {
    const std::filesystem::path dir = feed_handler::test_support::UniqueTestDir("book_concurrency");
    {
        BookRing ring_a(kRoomyRing);
        BookRing ring_b(kRoomyRing);
        RingSink sink_a(ring_a);
        RingSink sink_b(ring_b);
        CaptureSession session_a(
            {.directory = dir / "a", .exchange = "kraken", .journal_mode = JournalMode::kThreaded});
        CaptureSession session_b(
            {.directory = dir / "b", .exchange = "kraken", .journal_mode = JournalMode::kThreaded});
        session_a.AddSink(sink_a);
        session_b.AddSink(sink_b);

        RingAudit audit_a{.tag = 'A'};
        RingAudit audit_b{.tag = 'B'};
        std::atomic<int> producers_done{0};
        std::size_t failures_a = 0;
        std::size_t failures_b = 0;
        std::thread consumer(
            [&] { ConsumeBoth(ring_a, ring_b, producers_done, audit_a, audit_b); });
        StartingGate gate;
        std::thread producer_a = Spawn(gate, [&] {
            failures_a = RunCapture(session_a, 'A');
            producers_done.fetch_add(1, std::memory_order_release);
        });
        std::thread producer_b = Spawn(gate, [&] {
            failures_b = RunCapture(session_b, 'B');
            producers_done.fetch_add(1, std::memory_order_release);
        });
        gate.Open();
        producer_a.join();
        producer_b.join();
        consumer.join();

        EXPECT_EQ(failures_a, 0U);
        EXPECT_EQ(failures_b, 0U);
        ExpectCleanAudit(audit_a);
        ExpectCleanAudit(audit_b);
    }
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// A reconnect resets only the connection that reconnected.
// ---------------------------------------------------------------------------

void ExpectBothReady(const BookService& service) {
    ASSERT_NE(KrakenBookOf(service, 0), nullptr);
    ASSERT_NE(KrakenBookOf(service, 1), nullptr);
    EXPECT_EQ(KrakenBookOf(service, 0)->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(KrakenBookOf(service, 1)->GetReadiness(), Readiness::kReady);
}

// Connection A has just been told a reconnect (connect_id 2) and nothing since;
// connection B was not touched. A's books are gone and it is on the new
// connect_id; B is on its first, with the very same ready book object, and its
// counters have not moved.
void ExpectOnlyAWasReset(const BookService& service, const KrakenBook* b_book) {
    // Per connection: connect_id, stale, books, connects seen, disconnects seen.
    const auto view = [&service](std::size_t index) {
        const BookAdapter& adapter = service.Adapter();
        const ConnectionHandle handle = service.Handle(index);
        return std::array<std::uint64_t, 5>{
            adapter.ConnectId(handle), adapter.IsStale(handle) ? 1U : 0U, adapter.BookCount(handle),
            adapter.Stats(handle).connects, adapter.Stats(handle).disconnects};
    };
    EXPECT_EQ(view(0), (std::array<std::uint64_t, 5>{2, 0, 0, 2, 1})) << "A was not reset";
    EXPECT_EQ(view(1), (std::array<std::uint64_t, 5>{1, 0, 1, 1, 0})) << "B was touched";
    EXPECT_EQ(KrakenBookOf(service, 0), nullptr);
    EXPECT_EQ(KrakenBookOf(service, 1), b_book) << "B's book was rebuilt";
    EXPECT_EQ(b_book->GetReadiness(), Readiness::kReady);
}

// The exact interleaving, with no thread at all: connection A reconnects while
// connection B keeps its books, and the consumer is driven by Poll() between the
// steps, so every intermediate state can be looked at.
TEST(BookServiceConcurrency, AReconnectMidStreamResetsOnlyTheConnectionThatReconnected) {
    const std::vector<std::string> all = LongKrakenRun();
    const std::vector<std::string> first_a = FirstFrames(all, all.size() / 2);
    const std::vector<std::string> first_b = FirstFrames(all, all.size() / 3);
    const std::vector<std::string> again_a = FirstFrames(all, all.size() / 4);
    ASSERT_GT(first_b.size(), 0U);

    BookService service(BookService::Config{.ring_events = kRoomyRing});
    RingSink& sink_a = service.AddConnection("kraken-a", kKrakenSettings);
    RingSink& sink_b = service.AddConnection("kraken-b", kKrakenSettings);
    Produce(sink_a, first_a, 1, /*disconnect=*/false);
    Produce(sink_b, first_b, 1, /*disconnect=*/false);
    ASSERT_GT(service.Poll(), 0U);
    const KrakenBook* b_book = KrakenBookOf(service, 1);
    ASSERT_NE(b_book, nullptr);
    ASSERT_NE(KrakenBookOf(service, 0), nullptr);

    // A's socket drops and comes back: nothing else has been sent yet.
    sink_a.OnDisconnect(1);
    sink_a.OnConnect(2, "reconnect");
    ASSERT_GT(service.Poll(), 0U);
    ExpectOnlyAWasReset(service, b_book);

    // B keeps streaming through A's reconnect, and A starts over from a snapshot.
    for (std::size_t index = first_b.size(); index < all.size(); ++index) {
        sink_b.OnFrame(FrameOf(all[index], index + 1));
    }
    for (std::size_t index = 0; index < again_a.size(); ++index) {
        sink_a.OnFrame(FrameOf(again_a[index], index + 1));
    }
    service.Stop();

    InlineOracle expected_a(kKrakenSettings);
    expected_a.Connect(1);
    expected_a.Frames(first_a);
    expected_a.Disconnect(1);
    expected_a.Connect(2);
    expected_a.Frames(again_a);
    InlineOracle expected_b(kKrakenSettings);
    expected_b.Connect(1);
    expected_b.Frames(all);

    ExpectSameCounts(StatsOf(service, 0), expected_a.Stats());
    ExpectSameCounts(StatsOf(service, 1), expected_b.Stats());
    ExpectSameBook(KrakenBookOf(service, 0), KrakenBookOfOracle(expected_a));
    ExpectSameBook(KrakenBookOf(service, 1), KrakenBookOfOracle(expected_b));
    ExpectBothReady(service);
}

// The same scenario under load, from two real sessions on their own threads and a
// running book thread: A reconnects (BeginConnect again) in the middle of its
// stream while B streams on. The end state must be exactly what feeding each
// connection's own events inline gives.
struct ConcurrentReconnect {
    ConcurrentReconnect()
        : all(LongKrakenRun()),
          before(FirstFrames(all, all.size() / 2)),
          after(FirstFrames(all, all.size() / 4)) {
        expected_a.Connect(1);
        expected_a.Frames(before);
        expected_a.Disconnect(1);
        expected_a.Connect(2);
        expected_a.Frames(after);
        expected_a.Disconnect(2);
        expected_b.Connect(1);
        expected_b.Frames(all);
        expected_b.Disconnect(1);
    }

    std::vector<std::string> all;
    std::vector<std::string> before;
    std::vector<std::string> after;
    InlineOracle expected_a{kKrakenSettings};
    InlineOracle expected_b{kKrakenSettings};
};

// Streams `frames` into `session`; returns how many calls the session refused.
std::size_t Stream(CaptureSession& session, const std::vector<std::string>& frames) {
    std::size_t refused = 0;
    for (const std::string& frame : frames) {
        refused += session.OnWireMessage(BytesOf(frame), FrameSource::kKrakenJson) ? 0U : 1U;
    }
    return refused;
}

// A's whole capture: connect, `before`, reconnect, `after`, close. Returns the
// number of refused calls.
std::size_t RunReconnectingCapture(CaptureSession& session, const ConcurrentReconnect& scenario) {
    std::size_t refused = session.BeginConnect("first", FrameSource::kKrakenJson) ? 0U : 1U;
    refused += Stream(session, scenario.before);
    refused += session.BeginConnect("reconnect", FrameSource::kKrakenJson) ? 0U : 1U;
    refused += Stream(session, scenario.after);
    session.Close();
    return refused;
}

// B's whole capture: one connect, every frame, close.
std::size_t RunSteadyCapture(CaptureSession& session, const ConcurrentReconnect& scenario) {
    std::size_t refused = session.BeginConnect("only", FrameSource::kKrakenJson) ? 0U : 1U;
    refused += Stream(session, scenario.all);
    session.Close();
    return refused;
}

void RunConcurrentReconnectRound(const ConcurrentReconnect& scenario,
                                 const std::filesystem::path& dir) {
    BookService service(BookService::Config{.ring_events = kRoomyRing});
    RingSink& sink_a = service.AddConnection("kraken-a", kKrakenSettings);
    RingSink& sink_b = service.AddConnection("kraken-b", kKrakenSettings);
    std::atomic<std::size_t> refused{0};
    {
        CaptureSession session_a(
            {.directory = dir / "a", .exchange = "kraken", .journal_mode = JournalMode::kThreaded});
        CaptureSession session_b(
            {.directory = dir / "b", .exchange = "kraken", .journal_mode = JournalMode::kThreaded});
        session_a.AddSink(sink_a);
        session_b.AddSink(sink_b);
        service.Start();

        StartingGate gate;
        std::thread producer_a =
            Spawn(gate, [&] { refused += RunReconnectingCapture(session_a, scenario); });
        std::thread producer_b =
            Spawn(gate, [&] { refused += RunSteadyCapture(session_b, scenario); });
        gate.Open();
        producer_a.join();
        producer_b.join();
        service.Stop();
    }

    EXPECT_EQ(refused.load(), 0U);
    ASSERT_EQ(service.Dropped(0) + service.Dropped(1), 0U);
    EXPECT_EQ(service.InternalErrors(), 0U);
    // A ends on its second connect_id and B on its first; the counts (connects,
    // disconnects, frames, snapshots, issues) are the oracles'.
    const BookAdapter& adapter = service.Adapter();
    EXPECT_EQ(
        std::make_pair(adapter.ConnectId(service.Handle(0)), adapter.ConnectId(service.Handle(1))),
        std::make_pair(std::uint64_t{2}, std::uint64_t{1}));
    ExpectSameCounts(StatsOf(service, 0), scenario.expected_a.Stats());
    ExpectSameCounts(StatsOf(service, 1), scenario.expected_b.Stats());
    ExpectSameBook(KrakenBookOf(service, 0), KrakenBookOfOracle(scenario.expected_a));
    ExpectSameBook(KrakenBookOf(service, 1), KrakenBookOfOracle(scenario.expected_b));
}

TEST(BookServiceConcurrency, AConcurrentReconnectResetsOnlyItsOwnConnectionsBooks) {
    const ConcurrentReconnect scenario;
    const std::filesystem::path dir = feed_handler::test_support::UniqueTestDir("book_concurrency");
    for (std::size_t round = 0; round < kRounds; ++round) {
        SCOPED_TRACE("round " + std::to_string(round));
        RunConcurrentReconnectRound(scenario, dir / std::to_string(round));
    }
    std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace book_adapter
