// The journal on its own thread: what lets a connection thread hand a frame to
// the disk without ever touching it (docs/investigations/2026-09-21-issue-14-
// architecture.md, decisions/0004).
//
// The connection thread stamps a frame, copies it into an SPSC ring and returns;
// this thread drains the ring in order and runs an ordinary JournalWriter, so the
// bytes on disk are exactly what inline mode writes. Three things are different
// from inline and are the whole of this class:
//
//   * Failures are asynchronous. A ring that is full (the disk is slower than the
//     feed for longer than the ring holds) and a write that fails are both fatal
//     to the capture: the journal is the source of truth, so it neither blocks the
//     socket nor drops a frame. Either one latches a sticky error and calls the
//     fatal callback once.
//   * Closing a file is a barrier. Disconnect() returns only after this thread has
//     drained everything queued before it, flushed and closed the file.
//   * Control events (Connect, Disconnect) are never dropped, they use the ring's
//     unbounded push, so a full ring cannot lose the event that finishes a file.
//
// One instance serves one connection: exactly one producer thread calls Connect,
// Push, Disconnect and Start, and this class's own thread is the one consumer.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

#include "feed_handler/feed_event.h"
#include "feed_handler/journal_writer.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/spsc_ring.h"

namespace feed_handler {

/// Completion signal of one Disconnect: the producer waits on it, the journal
/// thread completes it once the file is closed.
class JournalCloseSignal {
  public:
    /// Journal thread: the file is closed and held `records` records.
    void Complete(std::uint64_t records) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            records_ = records;
            done_ = true;
        }
        cv_.notify_all();
    }

    /// Producer: blocks until Complete, returns the record count it carried.
    std::uint64_t Wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return done_; });
        return records_;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::uint64_t records_ = 0;
};

/// A new file: the writer (already open, header written, on the connection thread)
/// and the stamped connect marker that is to be its first record.
struct JournalConnect {
    std::unique_ptr<JournalWriter> writer;
    FrameEvent marker;
};

/// Flush and close the current file, then complete `signal`.
struct JournalDisconnect {
    std::shared_ptr<JournalCloseSignal> signal;
};

/// Drain what is queued and end the thread.
struct JournalStop {};

/// What the journal ring carries: a frame copy for the current file, or one of
/// the control events above. Frames are the FrameEvent of feed_event.h (its
/// `source` is not journaled, the file header carries the exchange instead).
using JournalEvent = std::variant<FrameEvent, JournalConnect, JournalDisconnect, JournalStop>;

class JournalThread {
  public:
    /// Called at most once, on whichever thread hit the failure: this class's own
    /// thread for a write failure, the producer's for a ring overflow. Must be
    /// thread safe. The reason never contains payload bytes.
    using FatalCallback = std::function<void(std::string_view reason)>;

    struct Config {
        /// Events the ring holds before Push reports an overflow. Size it for
        /// seconds of traffic.
        std::size_t ring_events = 1U << 16U;
        FatalCallback on_fatal = {};
        /// False leaves the thread unstarted until Start(), so nothing drains the
        /// ring: how a test fills it deterministically, without a sleep. The
        /// destructor starts it if nobody did, so queued events are still drained.
        bool start = true;
    };

    explicit JournalThread(Config cfg);

    JournalThread(const JournalThread&) = delete;
    JournalThread& operator=(const JournalThread&) = delete;
    JournalThread(JournalThread&&) = delete;
    JournalThread& operator=(JournalThread&&) = delete;

    /// Drains everything queued, closes an open file and joins the thread.
    ~JournalThread();

    /// Producer thread. Starts the thread if `Config::start` was false.
    void Start();

    /// Producer thread. Hands over a freshly opened file and copies `marker` (the
    /// stamped connect marker) to be written as its first record. Never fails and
    /// never blocks; a marker write failure is reported like any other.
    void Connect(std::unique_ptr<JournalWriter> writer, const CaptureFrame& marker);

    /// Producer thread. Copies `frame` into the ring. Returns false, without
    /// queueing, once the journal has failed; the push that finds the ring full is
    /// what fails it.
    bool Push(const CaptureFrame& frame);

    /// Producer thread. BARRIER: returns after the journal thread has written
    /// everything queued before it, flushed and closed the file. The file is then
    /// complete and readable. Returns how many records it held. Must follow a
    /// Connect. Starts the thread first if `Config::start` was false, since the
    /// barrier could otherwise never complete.
    std::uint64_t Disconnect();

    /// Any thread. True once an overflow or a write failure latched.
    bool Failed() const {
        return state_.load(std::memory_order_acquire) == kFailed;
    }

    /// Any thread. The sticky reason, empty while healthy. The string is written
    /// once, before Failed() turns true, and never changed after, so the view stays
    /// valid for the life of this object.
    std::string_view Error() const {
        return Failed() ? std::string_view(error_) : std::string_view{};
    }

    /// Any thread. Records the journal thread has written into the current file so
    /// far, marker included; 0 when no file is open. Lags Push by whatever is still
    /// queued.
    std::uint64_t RecordsWritten() const {
        return records_written_.load(std::memory_order_relaxed);
    }

    /// Any thread. How many frames the ring refused (0 or 1: the first refusal
    /// latches the failure and later frames are not offered to it).
    std::uint64_t Dropped() const {
        return ring_.Dropped();
    }

  private:
    // Idle waits are bounded so a frame wakeup that is missed (see Wake) costs a
    // delay, never a stall. Control events do not depend on this.
    static constexpr std::chrono::milliseconds kIdlePoll{20};

    enum : int { kHealthy = 0, kClaimed = 1, kFailed = 2 };

    void Run();
    void WaitForEvent(JournalEvent& event);
    /// `must` for events that must not wait out the idle poll.
    void Wake(bool must);
    void Fail(std::string_view reason);
    /// Publishes the record count and turns a writer failure into Fail().
    void AfterWrite();

    std::size_t capacity_;
    SpscRing<JournalEvent> ring_;
    FatalCallback on_fatal_;

    std::atomic<int> state_{kHealthy};
    /// Written by whoever wins the kHealthy -> kClaimed race, read only after
    /// state_ is kFailed.
    std::string error_;
    std::atomic<std::uint64_t> records_written_{0};

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    /// A hint that the journal thread is asleep (or about to be): frames only pay
    /// for a notify when it is set.
    std::atomic<bool> sleeping_{false};

    /// Journal thread only.
    std::unique_ptr<JournalWriter> writer_;

    /// Last, so every other member is constructed before the thread can run.
    std::thread thread_;
};

}  // namespace feed_handler
