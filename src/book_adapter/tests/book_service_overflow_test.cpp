// What happens to the live book path when the book ring cannot keep up: frames
// are dropped and counted, the producer is never held up, the affected books end
// desynced until a fresh connect_id brings a new snapshot, and the journal (which
// has its own ring and is the source of truth) does not lose a frame.
//
// Every overflow here is forced, not hoped for: the consumer is left unstarted, or
// driven with BookService::Poll(), so the ring is full or empty exactly when the
// test says. No assertion depends on a sleep or on who wins a race. The loops are
// fixed (a few hundred fixture frames, a few tens of KB in all) and nothing keeps
// payloads beyond what the assertion needs, so a bug cannot turn into a runaway
// (docs/tasks/testing.md).
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "book_adapter/book_service.h"
#include "book_adapter/tests/book_service_test_support.h"
#include "feed_handler/capture_session.h"
#include "feed_handler/journal_format.h"
#include "feed_handler/journal_reader.h"
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
using test_support::FirstFrames;
using test_support::FrameOf;
using test_support::InlineOracle;
using test_support::kKrakenSettings;
using test_support::kProducerBound;
using test_support::KrakenBookFrames;
using test_support::KrakenBookOf;
using test_support::kSomeFrames;
using test_support::kTinyRing;
using test_support::Produce;
using test_support::RunWithin;
using test_support::StatsOf;

// The frames of a Kraken stream: a snapshot and the updates continuing it, and
// (checked) no second snapshot, so a book that fell out of sync can only recover
// through a new connect_id.
std::vector<std::string> SingleSnapshotStream() {
    std::vector<std::string> frames = KrakenBookFrames();
    InlineOracle oracle(kKrakenSettings);
    oracle.Connect(1);
    oracle.Frames(frames);
    EXPECT_EQ(oracle.Stats().snapshots, 1U) << "the stream was meant to hold one snapshot";
    EXPECT_EQ(oracle.Stats().TotalIssues(), 0U) << "the stream was meant to apply cleanly";
    return frames;
}

// An unstarted service, a full ring behind it and a producer thread that has
// finished its fixed loop without the consumer ever running. The producer is
// bounded: if pushing into a full ring blocked, this fails instead of hanging.
struct OverflowedService {
    explicit OverflowedService(std::size_t connections = 1)
        : service(BookService::Config{.ring_events = kTinyRing}) {
        for (std::size_t index = 0; index < connections; ++index) {
            sinks.push_back(
                &service.AddConnection("kraken-" + std::to_string(index), kKrakenSettings));
        }
    }

    // Connect_id `connect_id` and all of `frames` from a producer thread. Returns
    // whether it finished in time; on a timeout the consumer is started so the
    // thread can end (and the test has already failed).
    [[nodiscard]] bool Flood(std::size_t index, const std::vector<std::string>& frames,
                             std::uint64_t connect_id) {
        return RunWithin(
            kProducerBound,
            [&] { Produce(*sinks[index], frames, connect_id, /*disconnect=*/false); },
            [&] { service.Start(); });
    }

    BookService service;
    std::vector<RingSink*> sinks;
};

TEST(BookRingOverflow, AProducerOutrunningAStalledConsumerNeverBlocksAndItsDropsAreCounted) {
    const std::vector<std::string> frames = SingleSnapshotStream();
    ASSERT_GT(frames.size(), kTinyRing);
    OverflowedService overflowed;
    BookService& service = overflowed.service;

    // The consumer does not run while the producer does, so the ring is full for
    // certain after its first few frames. Finishing inside the bound IS the
    // "never blocks" check: every push into a full ring returned.
    ASSERT_TRUE(overflowed.Flood(0, frames, 1)) << "the producer blocked on a full ring";
    ASSERT_FALSE(service.Running());
    EXPECT_EQ(StatsOf(service, 0).frames, 0U) << "nothing was consumed yet";

    const std::uint64_t dropped = service.Dropped(0);
    EXPECT_GT(dropped, 0U);
    EXPECT_LT(dropped, frames.size()) << "the ring accepted nothing";
    const std::uint64_t accepted = frames.size() - dropped;

    // The connect and every accepted frame come out; the dropped ones never do.
    EXPECT_EQ(service.Poll(), 1U + accepted);
    service.Stop();

    const ConnectionStats& stats = StatsOf(service, 0);
    EXPECT_EQ(stats.frames, accepted);
    EXPECT_EQ(stats.drops, dropped);
    EXPECT_EQ(stats.frames + stats.drops, frames.size());
    EXPECT_EQ(stats.snapshots, 1U) << "the snapshot went in before the ring filled";
    EXPECT_EQ(service.InternalErrors(), 0U);
}

TEST(BookRingOverflow, TheBooksOfAnOverflowedConnectionEndDesynced) {
    const std::vector<std::string> frames = SingleSnapshotStream();
    ASSERT_GT(frames.size(), kTinyRing);
    OverflowedService overflowed;
    BookService& service = overflowed.service;
    ASSERT_TRUE(overflowed.Flood(0, frames, 1)) << "the producer blocked on a full ring";
    ASSERT_GT(service.Dropped(0), 0U);

    // Stop() drains and also reports drops that no later entry carried.
    service.Stop();

    const KrakenBook* book = KrakenBookOf(service, 0);
    ASSERT_NE(book, nullptr) << "the snapshot was accepted, so the book exists";
    EXPECT_EQ(book->GetReadiness(), Readiness::kDesynced);
    EXPECT_FALSE(book->IsReady());
}

TEST(BookRingOverflow, AFollowingConnectIdWithAFreshSnapshotRecoversTheBooks) {
    const std::vector<std::string> frames = SingleSnapshotStream();
    ASSERT_GT(frames.size(), kTinyRing);
    OverflowedService overflowed;
    BookService& service = overflowed.service;

    // Connect_id 1 overflows the ring.
    ASSERT_TRUE(overflowed.Flood(0, frames, 1)) << "the producer blocked on a full ring";
    const std::uint64_t dropped = service.Dropped(0);
    ASSERT_GT(dropped, 0U);
    ASSERT_GT(service.Poll(), 0U);

    // The first frame after the storm carries the drops with it: the book learns
    // it missed frames and stays desynced, and does not heal by itself.
    RingSink& sink = *overflowed.sinks[0];
    sink.OnFrame(FrameOf(frames.back(), frames.size() + 1));
    ASSERT_GT(service.Poll(), 0U);
    EXPECT_EQ(StatsOf(service, 0).drops, dropped);
    const KrakenBook* desynced = KrakenBookOf(service, 0);
    ASSERT_NE(desynced, nullptr);
    EXPECT_EQ(desynced->GetReadiness(), Readiness::kDesynced);

    // The next natural reconnect: a new connect_id and a fresh snapshot.
    const std::vector<std::string> fresh = FirstFrames(frames, kSomeFrames);
    ASSERT_TRUE(RunWithin(
        kProducerBound,
        [&] {
            sink.OnDisconnect(1);
            Produce(sink, fresh, 2, /*disconnect=*/false);
        },
        [&] { service.Start(); }));
    ASSERT_GT(service.Poll(), 0U);
    service.Stop();

    EXPECT_EQ(service.Adapter().ConnectId(service.Handle(0)), 2U);
    EXPECT_FALSE(service.Adapter().IsStale(service.Handle(0)));
    EXPECT_EQ(service.Dropped(0), dropped) << "the new connect_id lost a frame too";
    EXPECT_EQ(StatsOf(service, 0).drops, dropped) << "the old drops were counted again";
    EXPECT_EQ(StatsOf(service, 0).connects, 2U);
    EXPECT_EQ(service.Adapter().BookCount(service.Handle(0)), 1U);

    // The recovered book is the book a clean run of the same frames builds.
    InlineOracle expected(kKrakenSettings);
    expected.Connect(2);
    expected.Frames(fresh);
    const KrakenBook* recovered = KrakenBookOf(service, 0);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->GetReadiness(), Readiness::kReady);
    ExpectSameBook(recovered, expected.Adapter().FindKrakenBook(expected.Handle(),
                                                                test_support::kKrakenSymbol));
}

TEST(BookRingOverflow, AnOverflowOnOneConnectionLeavesTheOthersBooksAlone) {
    const std::vector<std::string> frames = SingleSnapshotStream();
    ASSERT_GT(frames.size(), kTinyRing);
    OverflowedService overflowed(/*connections=*/2);
    BookService& service = overflowed.service;

    ASSERT_TRUE(overflowed.Flood(0, frames, 1)) << "the producer blocked on a full ring";
    const std::vector<std::string> calm = FirstFrames(frames, kSomeFrames);
    ASSERT_TRUE(overflowed.Flood(1, calm, 1));
    ASSERT_GT(service.Dropped(0), 0U);
    ASSERT_EQ(service.Dropped(1), 0U);
    service.Stop();

    EXPECT_GT(StatsOf(service, 0).drops, 0U);
    ASSERT_NE(KrakenBookOf(service, 0), nullptr);
    EXPECT_EQ(KrakenBookOf(service, 0)->GetReadiness(), Readiness::kDesynced);

    EXPECT_EQ(StatsOf(service, 1).drops, 0U);
    InlineOracle expected(kKrakenSettings);
    expected.Connect(1);
    expected.Frames(calm);
    test_support::ExpectSameCounts(StatsOf(service, 1), expected.Stats());
    ExpectSameBook(KrakenBookOf(service, 1), expected.Adapter().FindKrakenBook(
                                                 expected.Handle(), test_support::kKrakenSymbol));
    EXPECT_EQ(KrakenBookOf(service, 1)->GetReadiness(), Readiness::kReady);
}

// A consumer that IS running, against a ring far too small for the burst. How many
// frames it manages to keep up with is up to the scheduler, so nothing here says
// how many were dropped: only what has to hold whichever way the race goes.
TEST(BookRingOverflow, EveryFrameIsEitherAppliedOrCountedAsDroppedWhateverTheConsumerKeepsUpWith) {
    const std::vector<std::string> frames = SingleSnapshotStream();
    ASSERT_GT(frames.size(), kTinyRing);
    BookService service(BookService::Config{.ring_events = kTinyRing});
    RingSink& sink = service.AddConnection("kraken-a", kKrakenSettings);
    service.Start();

    ASSERT_TRUE(
        RunWithin(kProducerBound, [&] { Produce(sink, frames, 1, /*disconnect=*/true); }, [] {}));
    service.Stop();

    const ConnectionStats& stats = StatsOf(service, 0);
    EXPECT_EQ(stats.frames + stats.drops, frames.size());
    EXPECT_EQ(stats.drops, service.Dropped(0));
    EXPECT_EQ(stats.connects, 1U);
    EXPECT_EQ(stats.disconnects, 1U);
    EXPECT_EQ(service.InternalErrors(), 0U);
    // Only the snapshot can make a desynced book ready again, and there is one,
    // at the very start: so the book is desynced exactly when frames were lost.
    const KrakenBook* book = KrakenBookOf(service, 0);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->GetReadiness(), stats.drops > 0 ? Readiness::kDesynced : Readiness::kReady);
}

// ---------------------------------------------------------------------------
// The journal is unaffected.
// ---------------------------------------------------------------------------

struct JournalReadback {
    std::size_t connects = 0;
    std::vector<std::string> wire;
    std::vector<std::uint64_t> sequences;
    bool complete = false;
};

JournalReadback ReadJournal(const std::filesystem::path& path) {
    JournalReadback readback;
    auto reader = feed_handler::JournalReader::Open(path);
    EXPECT_TRUE(reader.has_value()) << "cannot read back " << path;
    if (!reader) {
        return readback;
    }
    while (const auto record = reader->Next()) {
        if (record->type == feed_handler::journal::RecordType::kConnect) {
            ++readback.connects;
        } else if (record->type == feed_handler::journal::RecordType::kWireMessage) {
            readback.wire.push_back(test_support::TextOf(record->payload));
            readback.sequences.push_back(record->capture_sequence);
        }
    }
    readback.complete = !reader->StoppedEarly();
    return readback;
}

// What a session that fed a full book ring did: where its journal is and how the
// calls went.
struct SessionRun {
    bool in_time = false;
    std::filesystem::path journal_path;
    std::size_t accepted_by_journal = 0;
    std::uint64_t fatal_calls = 0;
    std::string error;
};

// Runs one session, connect to close, on a producer thread into `sink` (behind an
// unstarted service: the book ring overflows for certain). The session lives only
// inside this call, so it is gone before the caller's service is.
SessionRun RunSession(BookService& service, RingSink& sink, JournalMode mode,
                      const std::filesystem::path& dir, const std::vector<std::string>& frames) {
    SessionRun run;
    CaptureSession session({.directory = dir, .exchange = "kraken", .journal_mode = mode});
    std::atomic<std::uint64_t> fatal_calls{0};
    session.SetFatalHandler([&fatal_calls](std::string_view) { ++fatal_calls; });
    session.AddSink(sink);
    run.in_time = RunWithin(
        kProducerBound,
        [&] {
            const auto began = session.BeginConnect("test", FrameSource::kKrakenJson);
            if (began) {
                run.journal_path = *began;
            }
            for (const std::string& frame : frames) {
                run.accepted_by_journal +=
                    session.OnWireMessage(BytesOf(frame), FrameSource::kKrakenJson) ? 1U : 0U;
            }
            session.Close();
        },
        [&] { service.Start(); });
    run.fatal_calls = fatal_calls.load();
    run.error = std::string(session.Error());
    return run;
}

// Every frame is in the file, in order, with consecutive sequence numbers (the
// connect marker took number 1), whatever the books lost.
void ExpectJournalHoldsEveryFrame(const std::filesystem::path& journal_path,
                                  const std::vector<std::string>& frames) {
    ASSERT_FALSE(journal_path.empty());
    const JournalReadback readback = ReadJournal(journal_path);
    EXPECT_TRUE(readback.complete);
    EXPECT_EQ(readback.connects, 1U);
    ASSERT_EQ(readback.wire.size(), frames.size());
    EXPECT_TRUE(readback.wire == frames) << "the journal holds different bytes than were sent";
    std::vector<std::uint64_t> expected_sequences(frames.size());
    std::ranges::iota(expected_sequences, std::uint64_t{2});
    EXPECT_TRUE(readback.sequences == expected_sequences) << "the sequence numbers are not 2..N+1";
}

// The session saw nothing wrong: every call succeeded, no fatal, no error.
void ExpectSessionHealthy(const SessionRun& run, std::size_t frames) {
    EXPECT_EQ(run.accepted_by_journal, frames);
    EXPECT_TRUE(run.error.empty()) << run.error;
    EXPECT_EQ(run.fatal_calls, 0U);
}

// The books lost frames the journal kept: what they saw plus what they dropped is
// what was sent.
void ExpectBooksLostFrames(const BookService& service, std::size_t frames) {
    const ConnectionStats& stats = StatsOf(service, 0);
    EXPECT_EQ(stats.frames + stats.drops, frames);
    EXPECT_GT(stats.drops, 0U);
    EXPECT_LT(stats.frames, frames) << "the books saw as many as the journal";
}

// Runs a session into an unstarted book service with a tiny ring, so the book ring
// overflows for certain, and checks the journal it wrote by reading it back.
void ExpectEveryFrameJournaledWhileTheBookRingOverflows(JournalMode mode) {
    const std::vector<std::string> frames = SingleSnapshotStream();
    ASSERT_GT(frames.size(), kTinyRing);
    const std::filesystem::path dir = feed_handler::test_support::UniqueTestDir("book_overflow");
    {
        BookService service(BookService::Config{.ring_events = kTinyRing});
        RingSink& sink = service.AddConnection("kraken-a", kKrakenSettings);
        const SessionRun run = RunSession(service, sink, mode, dir, frames);
        ASSERT_TRUE(run.in_time) << "the producer blocked on a full ring";

        // The book ring overflowed, and that is not a journal failure.
        ExpectSessionHealthy(run, frames.size());
        ExpectJournalHoldsEveryFrame(run.journal_path, frames);
        service.Stop();
        ExpectBooksLostFrames(service, frames.size());
    }
    std::filesystem::remove_all(dir);
}

TEST(BookRingOverflow, TheInlineJournalKeepsEveryFrameWhileTheBookRingOverflows) {
    ExpectEveryFrameJournaledWhileTheBookRingOverflows(JournalMode::kInline);
}

TEST(BookRingOverflow, TheThreadedJournalKeepsEveryFrameWhileTheBookRingOverflows) {
    ExpectEveryFrameJournaledWhileTheBookRingOverflows(JournalMode::kThreaded);
}

}  // namespace
}  // namespace book_adapter
