// The live book path: one thread that turns every connection's ring into books
// (docs/investigations/2026-09-21-issue-14-architecture.md).
//
// A BookService owns one BookAdapter and one thread. Each connection it is given
// gets a ring and the RingSink that fills it; the book thread polls the rings
// round-robin, pops events in order and hands them to the adapter, which parses
// and applies them. Nothing here is on a connection's thread except the sink.
//
// Lifecycle, and who may touch what when:
//
//   1. AddConnection() for each connection, then Start(). Registration is over
//      once the thread runs: it walks the connection list without a lock.
//   2. The connections produce into their sinks (their own threads).
//   3. Stop(), only after every producer has ended (the connections are joined).
//      It drains every ring, then ends the thread. The destructor calls it.
//   4. The adapter and the summaries are readable while no thread runs: before
//      Start() and after Stop(). While it runs the adapter belongs to the book
//      thread.
//
// A book bug must not take the capture down. The service latches nothing fatal
// and has no way to change the process's exit code: the adapter counts and logs
// what goes wrong, and an exception that still escapes it is caught here, logged
// and counted.
//
// The thread never blocks on a lock or a condition variable. It spins briefly
// when its rings are empty, then yields, then sleeps for a moment, so an idle
// capture costs almost nothing and a busy one is served with no wakeup latency.
// Stop() is a flag and a join, so it never waits on a signal that can be lost.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "book_adapter/book_adapter.h"
#include "book_adapter/book_settings.h"
#include "book_adapter/ring_sink.h"

namespace book_adapter {

class BookService {
  public:
    struct Config {
        /// Events each connection's ring holds before it starts dropping frames.
        /// Size it for seconds of traffic: a drop desyncs the books until the
        /// next snapshot. Control events can exceed it.
        std::size_t ring_events = 1U << 16U;
        BookAdapter::Config adapter = {};
    };

    explicit BookService(const Config& config);

    BookService(const BookService&) = delete;
    BookService& operator=(const BookService&) = delete;
    BookService(BookService&&) = delete;
    BookService& operator=(BookService&&) = delete;

    /// Stop() if nobody has, so a service abandoned by an early return still
    /// drains its rings and joins its thread. Every producer must be gone by now.
    ~BookService();

    /// Registers a connection and returns the sink to hand to its CaptureSession
    /// (CaptureConnection::AddSink). `name` labels its log lines and summary. The
    /// sink lives as long as this service. Only before Start().
    RingSink& AddConnection(std::string name, const BookSettings& settings);

    /// Starts the book thread. A no-op, and no thread, when no connection was
    /// added: a capture without books runs no book code. Only once.
    void Start();

    /// Drains every ring, then ends the thread. Call it once every producer has
    /// ended: nothing pushed after it is looked at, and the drain relies on the
    /// producers' last push being visible, which joining them guarantees. Works
    /// without Start() too (it then drains on the calling thread). Idempotent.
    void Stop();

    /// Handles everything currently queued, on the calling thread, until every
    /// ring is empty; returns how many events that was. For a service that is not
    /// running a thread (embedding it in a single-threaded loop, and tests that
    /// need an exact interleaving of pushes and pops): never once Start() created
    /// one, since the rings have one consumer.
    std::size_t Poll();

    /// True from Start() until Stop() when a thread exists.
    [[nodiscard]] bool Running() const {
        return thread_.joinable();
    }

    [[nodiscard]] std::size_t ConnectionCount() const {
        return feeds_.size();
    }

    /// The adapter, for inspecting books and counters. Only while no thread runs
    /// (before Start() or after Stop()).
    [[nodiscard]] const BookAdapter& Adapter() const {
        return adapter_;
    }

    /// The handle of the connection AddConnection returned the `index`th sink for.
    [[nodiscard]] ConnectionHandle Handle(std::size_t index) const {
        return feeds_[index]->handle;
    }

    /// Frames the connection's ring refused, over the service's life. Any thread.
    [[nodiscard]] std::uint64_t Dropped(std::size_t index) const {
        return feeds_[index]->ring.Dropped();
    }

    /// One line for the log: what the books of connection `index` saw. Only while
    /// no thread runs.
    [[nodiscard]] std::string Summary(std::size_t index) const;

    /// Logs Summary() of every connection at info level. Only while no thread
    /// runs.
    void LogSummaries() const;

    /// Exceptions that escaped the adapter and were swallowed, over all
    /// connections. Zero in a healthy run. Any thread.
    [[nodiscard]] std::uint64_t InternalErrors() const {
        return internal_errors_.load(std::memory_order_relaxed);
    }

  private:
    /// One connection: its ring, the sink filling it, and the adapter handle the
    /// consumer applies its events to. Not movable (the sink points at the ring).
    struct Feed {
        Feed(const Config& config, ConnectionHandle connection)
            : ring(config.ring_events), sink(ring), handle(connection) {}

        BookRing ring;
        RingSink sink;
        ConnectionHandle handle;
        /// Drops already passed to the adapter. Consumer side only.
        std::uint64_t reported_drops = 0;
    };

    /// Events taken from one ring per pass before moving to the next, so a ring
    /// with a backlog cannot starve the others.
    static constexpr std::size_t kBatch = 64;

    /// Only this many swallowed exceptions are logged; the counter keeps going.
    static constexpr std::uint64_t kMaxLoggedInternalErrors = 8;

    void Run();
    /// One pass over every ring; returns how many events it handled.
    std::size_t DrainPass();
    std::size_t Drain(Feed& feed);
    void Dispatch(Feed& feed, const RingEntry& entry);
    /// Tells the adapter about drops up to `total` that it has not heard of yet.
    void ReportDrops(Feed& feed, std::uint64_t total);
    /// After the last event of a ring: drops that no later entry carried because
    /// the ring ended on them.
    void ReportTrailingDrops(Feed& feed);
    void CountInternalError(const Feed& feed, const char* what);
    static void Backoff(std::size_t idle_passes);

    Config config_;
    BookAdapter adapter_;
    std::vector<std::unique_ptr<Feed>> feeds_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> internal_errors_{0};
    bool stopped_ = false;
    /// Last, so every other member exists before the thread can run.
    std::thread thread_;
};

}  // namespace book_adapter
