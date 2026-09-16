// Application-level "the feed has gone silent" timer.
//
// decisions/0004: a half-open TCP connection produces silence, not an error,
// and relying on the transport library's own heartbeat means relying on an
// off-by-default setting. This is the independent check: no inbound traffic
// within the timeout means the connection is dead even if the socket still
// looks open, and the connection has to be torn down and re-established.
//
// Deliberately clock-free: the caller passes in the current CLOCK_MONOTONIC
// reading, which keeps the timing logic testable without a live socket or a
// sleeping test.
#pragma once

#include <atomic>
#include <cstdint>

namespace feed_handler {

/// Not a timer thread of its own -- a piece of state a poller consults.
/// note_activity() runs on the connection's thread while is_stale() runs on
/// whichever thread polls, so the state is atomic.
class StalenessWatchdog {
  public:
    explicit StalenessWatchdog(std::uint64_t timeout_ns) : timeout_ns_(timeout_ns) {}

    /// Records inbound traffic at `now_ns`, arming the watchdog if it was
    /// disarmed. Called for every inbound frame, including transport-level
    /// pongs: any byte from the peer proves the connection is not half-open.
    void NoteActivity(std::uint64_t now_ns) {
        last_activity_ns_.store(now_ns, std::memory_order_relaxed);
        armed_.store(true, std::memory_order_release);
    }

    /// Stops the watchdog firing until the next note_activity(). Used while
    /// deliberately disconnected: silence during a reconnect is expected, and
    /// a still-armed watchdog would otherwise fire again and again, tearing
    /// down each new connection before it had a chance to deliver anything.
    void Disarm() {
        armed_.store(false, std::memory_order_release);
    }

    bool Armed() const {
        return armed_.load(std::memory_order_acquire);
    }

    /// True once `timeout_ns` has elapsed since the last recorded activity.
    /// A zero timeout disables the check entirely.
    bool IsStale(std::uint64_t now_ns) const {
        if (timeout_ns_ == 0 || !Armed()) {
            return false;
        }
        const std::uint64_t last = last_activity_ns_.load(std::memory_order_relaxed);
        return now_ns > last && (now_ns - last) >= timeout_ns_;
    }

    std::uint64_t TimeoutNs() const {
        return timeout_ns_;
    }

    std::uint64_t LastActivityNs() const {
        return last_activity_ns_.load(std::memory_order_relaxed);
    }

  private:
    std::uint64_t timeout_ns_;
    std::atomic<std::uint64_t> last_activity_ns_{0};
    std::atomic<bool> armed_{false};
};

}  // namespace feed_handler
